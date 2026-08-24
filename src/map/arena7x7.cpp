// Copyright (c) rAthena Dev Teams - Licensed under GNU GPL
// For more information, see LICENCE in the main folder

/**
 * =============================================================================
 * Arena 7x7 - Sistema de Batalha PvP em Equipes
 * =============================================================================
 *
 * Este m�dulo implementa um sistema de arena PvP onde duas equipes de at� 7
 * jogadores cada se enfrentam. O sistema inclui:
 *
 * - Gerenciamento de partidas (criar, iniciar, finalizar, cancelar)
 * - Tracking detalhado de estat�sticas (dano, kills, assists, healing)
 * - Sistema de morte permanente (jogador vira tumba at� o fim da partida)
 * - Persist�ncia em banco de dados SQL
 * - Integra��o com sistema de ranking e seasons
 * - Logs detalhados para an�lise no site
 *
 * Configura��o:
 * - M�ximo de jogadores por time: definido em ARENA7X7_MAX_PLAYERS_PER_TEAM
 * - Janela de assist�ncia: ARENA7X7_ASSIST_WINDOW (5 segundos)
 *
 * =============================================================================
 */

#include "arena7x7.hpp"

#include <algorithm>
#include <cstring>
#include <unordered_set>

#include <common/nullpo.hpp>
#include <common/showmsg.hpp>
#include <common/sql.hpp>
#include <common/strlib.hpp>
#include <common/timer.hpp>
#include <common/utilities.hpp>
#include <common/utils.hpp>

#include "battle.hpp"
#include "battleground.hpp"
#include "clif.hpp"
#include "guild.hpp"
#include "itemdb.hpp"
#include "map.hpp"
#include "mob.hpp"
#include "npc.hpp"
#include "pc.hpp"
#include "skill.hpp"
#include "status.hpp"
#include "unit.hpp"

using namespace rathena;

// ============================================================================
// Vari�veis Globais
// ============================================================================

/// Mapa de todas as partidas ativas indexadas por match_id
/// Usa shared_ptr para gerenciamento autom�tico de mem�ria
std::unordered_map<uint32, std::shared_ptr<s_arena7x7_match>> arena7x7_matches;

/// Mapa r�pido para encontrar qual partida um jogador est�
/// Formato: char_id -> match_id
std::unordered_map<uint32, uint32> arena7x7_player_match;

/// Contador incremental para gerar IDs �nicos de partidas
/// Inicializado do banco de dados no startup
uint32 arena7x7_match_counter = 0;

/// Temporada atual do sistema de ranking
/// Carregada do banco de dados no startup
uint16 arena7x7_current_season = 1;

// ============================================================================
// Fun��es de Gerenciamento de Partida
// ============================================================================

/**
 * Cria uma nova partida
 */
uint32 arena7x7_create_match(const char *map_name)
{
	if (!map_name || !map_name[0])
	{
		// showerror("arena7x7_create_match: nome do mapa invalido\n");
		return 0;
	}

	// Incrementar contador (ser� inicializado do SQL no init)
	uint32 new_id = ++arena7x7_match_counter;

	auto match = std::make_shared<s_arena7x7_match>();
	match->match_id = new_id;
	match->map_name = map_name;
	match->status = ARENA7X7_MATCH_WAITING;
	match->season = arena7x7_current_season;

	arena7x7_matches[new_id] = match;

	// #if ARENA7X7_DEBUG
	// 	ShowInfo("Arena7x7: Partida %u criada no mapa %s\n", new_id, map_name);
	// #endif
	return new_id;
}

/**
 * Adiciona um jogador � partida
 */
bool arena7x7_add_player(uint32 match_id, map_session_data *sd, e_arena7x7_team team, bool is_leader)
{
	nullpo_retr(false, sd);

	// Validar ponteiro do status do jogador
	if (!sd->status.name || strlen(sd->status.name) == 0)
	{
		// showerror("arena7x7_add_player: nome do jogador invalido (char_id: %u)\n", sd->status.char_id);
		return false;
	}

	auto it = arena7x7_matches.find(match_id);
	if (it == arena7x7_matches.end())
	{
		// //showerror("arena7x7_add_player: partida %u nao encontrada\n", match_id);
		return false;
	}

	// Guardar refer�ncia ao shared_ptr para evitar problemas se o iterador for invalidado
	std::shared_ptr<s_arena7x7_match> match = it->second;

	// Validar se o ponteiro do match � v�lido (verificar ponteiro interno tamb�m)
	if (!match || !match.get())
	{
		// //showerror("arena7x7_add_player: ponteiro de partida %u invalido (match=%p, get=%p)\n",
		// 		  match_id, (void *)&match, (void *)match.get());
		return false;
	}

	// Verificar se partida ainda aceita jogadores
	if (match->status != ARENA7X7_MATCH_WAITING && match->status != ARENA7X7_MATCH_ACTIVE)
	{
		// showwarning("arena7x7_add_player: partida %u nao aceita mais jogadores\n", match_id);
		return false;
	}

	// Verificar se jogador j� est� em alguma partida
	if (arena7x7_player_match.find(sd->status.char_id) != arena7x7_player_match.end())
	{
		// showwarning("arena7x7_add_player: jogador %s ja esta em uma partida\n", sd->status.name);
		return false;
	}

	// Criar estat�sticas do jogador
	auto stats = std::make_shared<s_arena7x7_player_stats>();

	// Validar se a aloca��o foi bem sucedida
	if (!stats || !stats.get())
	{
		// //showerror("arena7x7_add_player: falha ao alocar memoria para estatisticas do jogador %s\n", sd->status.name);
		return false;
	}

	stats->char_id = sd->status.char_id;
	stats->account_id = sd->status.account_id;

	// Garantir que o nome est� null-terminated antes de usar
	char safe_name[NAME_LENGTH + 1];
	memset(safe_name, 0, sizeof(safe_name)); // Zerar todo o buffer
	safestrncpy(safe_name, sd->status.name, sizeof(safe_name));

	// Criar string de forma segura
	try
	{
		stats->char_name.assign(safe_name, strnlen(safe_name, NAME_LENGTH));
	}
	catch (const std::exception &)
	{
		// //showerror("arena7x7_add_player: falha ao copiar nome do jogador\n");
		return false;
	}

	stats->team = team;
	stats->job_class = sd->status.class_;
	stats->base_level = sd->status.base_level;
	stats->is_leader = is_leader;
	stats->is_deserter = false;
	stats->join_time = gettick();

	// Capturar informa��es da guild do jogador
	stats->guild_id = sd->status.guild_id;
	if (sd->status.guild_id > 0)
	{
		auto g = guild_search(sd->status.guild_id);
		if (g && g->guild.name)
		{
			// Garantir que o nome da guild est� null-terminated
			char safe_guild_name[NAME_LENGTH + 1];
			memset(safe_guild_name, 0, sizeof(safe_guild_name)); // Zerar todo o buffer
			safestrncpy(safe_guild_name, g->guild.name, sizeof(safe_guild_name));

			// Criar string de forma segura
			try
			{
				stats->guild_name.assign(safe_guild_name, strnlen(safe_guild_name, NAME_LENGTH));
			}
			catch (const std::exception &)
			{
				// //showerror("arena7x7_add_player: falha ao copiar nome da guild\n");
				stats->guild_name = ""; // Deixar vazio em caso de erro
			}

			// Atualizar nome da guild do time se ainda n�o foi definido
			if (team == ARENA7X7_TEAM_BLUE && match->blue_guild_id == 0)
			{
				match->blue_guild_id = sd->status.guild_id;
				match->blue_guild_name = stats->guild_name;
			}
			else if (team == ARENA7X7_TEAM_RED && match->red_guild_id == 0)
			{
				match->red_guild_id = sd->status.guild_id;
				match->red_guild_name = stats->guild_name;
			}
		}
	}

	// Adicionar � partida
	match->players[sd->status.char_id] = stats;
	arena7x7_player_match[sd->status.char_id] = match_id;

	// Incrementar contador de jogadores vivos para atualiza��o do placar
	if (team == ARENA7X7_TEAM_BLUE)
	{
		match->blue_alive++;
	}
	else if (team == ARENA7X7_TEAM_RED)
	{
		match->red_alive++;
	}

	// #if ARENA7X7_DEBUG
	// 	ShowInfo("Arena7x7: Jogador %s adicionado a partida %u (time %s%s)\n",
	// 			 sd->status.name, match_id,
	// 			 team == ARENA7X7_TEAM_BLUE ? "Azul" : "Vermelho",
	// 			 is_leader ? ", Lider" : "");
	// #endif

	return true;
}

/**
 * Remove um jogador da partida
 * A restaura��o do t�mulo � feita em pc_setpos quando o jogador muda de mapa
 */
bool arena7x7_remove_player(map_session_data *sd, bool deserter)
{
	nullpo_retr(false, sd);

	auto it = arena7x7_player_match.find(sd->status.char_id);
	if (it == arena7x7_player_match.end())
	{
		return false; // N�o est� em partida
	}

	uint32 match_id = it->second;
	auto match = arena7x7_get_match(match_id);
	if (!match)
	{
		arena7x7_player_match.erase(it);
		return false;
	}

	// Marcar como desertor se aplic�vel
	auto pit = match->players.find(sd->status.char_id);
	if (pit != match->players.end() && pit->second)
	{
		pit->second->is_deserter = deserter;

		// Se for desertor E a partida estiver ativa, aplicar penalidade imediatamente
		if (deserter && match->status == ARENA7X7_MATCH_ACTIVE)
		{
			// Aplicar penalidade de -3 pontos e +1 morte
			pit->second->deaths += 1; // Contar como morte
			arena7x7_update_ranking(sd->status.char_id, pit->second.get(), false, true, false);

			// #if ARENA7X7_DEBUG
			// 			ShowInfo("Arena7x7: Desertor %s penalizado (-3 pontos, +1 death) na partida %u\n",
			// 					 sd->status.name, match_id);
			// #endif
		}
	}

	// Remover do mapa de jogadores (mas manter nas estat�sticas da partida para hist�rico)
	arena7x7_player_match.erase(it);

	// #if ARENA7X7_DEBUG
	// 	if (deserter)
	// 	{
	// 		ShowInfo("Arena7x7: Jogador %s desertou da partida %u\n", sd->status.name, match_id);
	// 	}
	// 	else
	// 	{
	// 		ShowInfo("Arena7x7: Jogador %s removido da partida %u\n", sd->status.name, match_id);
	// 	}
	// #endif

	return true;
}

/**
 * Inicia uma partida
 */
bool arena7x7_start_match(uint32 match_id)
{
	auto match = arena7x7_get_match(match_id);
	if (!match)
	{
		// //showerror("arena7x7_start_match: partida %u nao encontrada\n", match_id);
		return false;
	}

	if (match->status != ARENA7X7_MATCH_WAITING)
	{
		// showwarning("arena7x7_start_match: partida %u nao esta em espera\n", match_id);
		return false;
	}

	match->status = ARENA7X7_MATCH_ACTIVE;
	match->start_time = gettick();

	// Inicializar contagem de jogadores vivos e atualizar placar do BG
	arena7x7_count_alive_players(match);
	arena7x7_update_score_display(match);

	// #if ARENA7X7_DEBUG
	// 	ShowInfo("Arena7x7: Partida %u iniciada com %zu jogadores (Azul: %u, Vermelho: %u)\n",
	// 			 match_id, match->players.size(), match->blue_alive, match->red_alive);
	// #endif
	return true;
}

/**
 * Finaliza uma partida
 */
bool arena7x7_finish_match(uint32 match_id, e_arena7x7_team winner_team, uint16 blue_score, uint16 red_score)
{
	auto match = arena7x7_get_match(match_id);
	if (!match)
	{
		// //showerror("arena7x7_finish_match: partida %u nao encontrada\n", match_id);
		return false;
	}

	match->status = ARENA7X7_MATCH_FINISHED;
	match->winner_team = winner_team;
	match->blue_score = blue_score;
	match->red_score = red_score;
	match->end_time = gettick();

	// Calcular dura��o da partida em segundos
	match->duration_seconds = (uint32)(DIFF_TICK(match->end_time, match->start_time) / 1000);

	// Calcular tempo vivo para jogadores que sobreviveram a partida inteira
	t_tick now = gettick();
	for (auto &kv : match->players)
	{
		auto &stats = kv.second;
		if (!stats)
			continue;

		// Se time_alive ainda � 0, significa que o jogador n�o morreu
		// Ent�o o tempo vivo � desde join_time at� agora (fim da partida)
		if (stats->time_alive == 0)
		{
			stats->time_alive = (uint32)DIFF_TICK(now, stats->join_time);
		}
	}

	// #if ARENA7X7_DEBUG
	// 	ShowInfo("Arena7x7: Partida %u finalizada - Vencedor: %s (Azul %u x %u Vermelho)\n",
	// 			 match_id,
	// 			 winner_team == ARENA7X7_TEAM_BLUE ? "Azul" : (winner_team == ARENA7X7_TEAM_RED ? "Vermelho" : "Empate"),
	// 			 blue_score, red_score);
	// #endif

	// Salvar no banco de dados
	if (!arena7x7_save_match(match))
	{
		// //showerror("arena7x7_finish_match: falha ao salvar partida %u no banco\n", match_id);
	}

	// Salvar logs detalhados (skills, itens, ataques)
	arena7x7_save_detailed_logs(match_id);

	// N�O restaurar jogadores aqui - ser�o restaurados em arena7x7_remove_player
	// quando o script teleportar cada jogador individualmente
	// Isso mant�m os jogadores como t�mulo at� o momento do teleporte

	// Atualizar ranking para cada jogador
	for (auto &kv : match->players)
	{
		auto &stats = kv.second;
		if (!stats)
			continue;

		// Se � desertor e j� foi penalizado (n�o est� mais no player_match), pular
		// Isso evita aplicar dupla penalidade
		if (stats->is_deserter && arena7x7_player_match.find(stats->char_id) == arena7x7_player_match.end())
		{
			// #if ARENA7X7_DEBUG
			// 			ShowInfo("Arena7x7: Desertor %s j� foi penalizado, pulando atualiza��o no finish\n",
			// 					 stats->char_name.c_str());
			// #endif
			continue; // J� foi penalizado em arena7x7_remove_player
		}

		// Desertor � sempre tratado como derrota, n�o importa o resultado do time
		bool won = (stats->is_deserter) ? false : (stats->team == winner_team);
		bool lost = (stats->is_deserter) ? true : (winner_team != ARENA7X7_TEAM_NONE && stats->team != winner_team);
		bool tie = (stats->is_deserter) ? false : (winner_team == ARENA7X7_TEAM_NONE);

		arena7x7_update_ranking(stats->char_id, stats.get(), won, lost, tie);
	}

	// N�O remover jogadores aqui - ser�o removidos em pc_setpos quando teleportarem
	// Isso permite que a restaura��o do t�mulo aconte�a corretamente

	return true;
}

/**
 * Cancela uma partida
 * Se cancelada durante prepara��o (antes de iniciar), remove completamente e libera o ID
 * N�O remove jogadores aqui - ser�o removidos em pc_setpos quando teleportarem
 */
bool arena7x7_cancel_match(uint32 match_id)
{
	auto match = arena7x7_get_match(match_id);
	if (!match)
	{
		// //showerror("arena7x7_cancel_match: partida %u nao encontrada\n", match_id);
		return false;
	}

	// Se a partida ainda estava em prepara��o (nunca iniciou), deletar completamente
	// para liberar o match_id e n�o "pular" IDs
	if (match->status == ARENA7X7_MATCH_WAITING)
	{
		// Remover todos os jogadores da partida
		for (auto it = arena7x7_player_match.begin(); it != arena7x7_player_match.end();)
		{
			if (it->second == match_id)
				it = arena7x7_player_match.erase(it);
			else
				++it;
		}

		// Remover a partida do mapa
		arena7x7_matches.erase(match_id);

		// Se for a �ltima partida criada, decrementar o contador para reutilizar o ID
		if (match_id == arena7x7_match_counter)
		{
			arena7x7_match_counter--;
			// #if ARENA7X7_DEBUG
			// 			ShowInfo("Arena7x7: Partida %u cancelada durante preparacao - ID liberado (proximo: %u)\n",
			// 					 match_id, arena7x7_match_counter + 1);
			// #endif
		}
		else
		{
			// #if ARENA7X7_DEBUG
			// 			ShowInfo("Arena7x7: Partida %u cancelada durante preparacao e removida\n", match_id);
			// #endif
		}

		return true;
	}

	// Se a partida j� tinha iniciado, apenas marca como cancelada (mant�m no hist�rico)
	match->status = ARENA7X7_MATCH_CANCELLED;
	match->end_time = gettick();

	// #if ARENA7X7_DEBUG
	// 	ShowInfo("Arena7x7: Partida %u cancelada ap�s in�cio\n", match_id);
	// #endif
	return true;
}

/**
 * Define os nomes das guilds para uma partida
 * Pode ser chamado pelo NPC quando os times s�o formados
 */
bool arena7x7_set_guild_names(uint32 match_id, const char *blue_guild_name, const char *red_guild_name, uint32 blue_guild_id, uint32 red_guild_id)
{
	auto match = arena7x7_get_match(match_id);
	if (!match)
	{
		// showerror("arena7x7_set_guild_names: partida %u nao encontrada\n", match_id);
		return false;
	}

	if (blue_guild_name && blue_guild_name[0])
	{
		char safe_blue_name[NAME_LENGTH + 1];
		memset(safe_blue_name, 0, sizeof(safe_blue_name));
		safestrncpy(safe_blue_name, blue_guild_name, sizeof(safe_blue_name));
		try
		{
			match->blue_guild_name.assign(safe_blue_name, strnlen(safe_blue_name, NAME_LENGTH));
		}
		catch (const std::exception &)
		{
			// showerror("arena7x7_set_guild_names: falha ao copiar nome da guild azul\n");
		}
		match->blue_guild_id = blue_guild_id;
	}

	if (red_guild_name && red_guild_name[0])
	{
		char safe_red_name[NAME_LENGTH + 1];
		memset(safe_red_name, 0, sizeof(safe_red_name));
		safestrncpy(safe_red_name, red_guild_name, sizeof(safe_red_name));
		try
		{
			match->red_guild_name.assign(safe_red_name, strnlen(safe_red_name, NAME_LENGTH));
		}
		catch (const std::exception &)
		{
			// showerror("arena7x7_set_guild_names: falha ao copiar nome da guild vermelha\n");
		}
		match->red_guild_id = red_guild_id;
	}

	// #if ARENA7X7_DEBUG
	// 	ShowInfo("Arena7x7: Partida %u - Times: %s vs %s\n",
	// 			 match_id, match->blue_guild_name.c_str(), match->red_guild_name.c_str());
	// #endif
	return true;
}

/**
 * Obt�m a partida de um jogador
 */
std::shared_ptr<s_arena7x7_match> arena7x7_get_player_match(uint32 char_id)
{
	auto it = arena7x7_player_match.find(char_id);
	if (it == arena7x7_player_match.end())
	{
		return nullptr;
	}
	return arena7x7_get_match(it->second);
}

/**
 * Obt�m uma partida pelo ID
 */
std::shared_ptr<s_arena7x7_match> arena7x7_get_match(uint32 match_id)
{
	auto it = arena7x7_matches.find(match_id);
	if (it == arena7x7_matches.end())
	{
		return nullptr;
	}
	return it->second;
}

/**
 * Verifica se um jogador est� em uma partida (ativa ou finalizada)
 * Inclui partidas finalizadas para permitir a restaura��o do t�mulo no momento do warp
 */
bool arena7x7_is_in_match(uint32 char_id)
{
	auto match = arena7x7_get_player_match(char_id);
	if (!match)
		return false;
	// Incluir FINISHED e CANCELLED para permitir restaura��o do t�mulo no warp
	return (match->status == ARENA7X7_MATCH_ACTIVE ||
			match->status == ARENA7X7_MATCH_WAITING ||
			match->status == ARENA7X7_MATCH_FINISHED ||
			match->status == ARENA7X7_MATCH_CANCELLED);
}

/**
 * Verifica se um jogador est� "morto" (tumba) na arena
 * Jogadores mortos/tumba n�o podem receber dispell, dano, etc.
 */
bool arena7x7_is_player_dead(uint32 char_id)
{
	auto stats = arena7x7_get_player_stats(char_id);
	if (!stats)
		return false;
#if ARENA7X7_DEBUG
	if (stats->is_dead)
		ShowInfo("Arena7x7 DEBUG: is_player_dead check - char_id=%u is DEAD\n", char_id);
#endif
	return stats->is_dead;
}

// ============================================================================
// Fun��es de Tracking de Estat�sticas
// ============================================================================

/**
 * Registra dano causado
 */
void arena7x7_record_damage(std::shared_ptr<s_arena7x7_match> match,
							uint32 attacker_id, uint32 target_id, uint16 skill_id, uint32 damage, bool is_critical)
{

	if (!match || match->status != ARENA7X7_MATCH_ACTIVE)
		return;

	t_tick now = gettick();

	// Log detalhado
	s_arena7x7_damage_entry entry;
	entry.attacker_id = attacker_id;
	entry.target_id = target_id;
	entry.skill_id = skill_id;
	entry.damage = damage;
	entry.is_critical = is_critical;
	entry.is_kill = false; // Ser� atualizado em record_kill se for o caso
	entry.timestamp = now;
	match->damage_log.push_back(entry);

	// Atualizar estat�sticas do atacante
	auto ait = match->players.find(attacker_id);
	if (ait != match->players.end() && ait->second)
	{
		auto &astats = ait->second;
		astats->damage_done += damage;
		if (damage > astats->top_damage)
		{
			astats->top_damage = damage;
		}
	}

	// Atualizar estat�sticas do alvo
	auto tit = match->players.find(target_id);
	if (tit != match->players.end() && tit->second)
	{
		auto &tstats = tit->second;
		tstats->damage_received += damage;

		// Registrar dano recente para assist�ncias
		s_arena7x7_recent_damage rd;
		rd.attacker_id = attacker_id;
		rd.skill_id = skill_id;
		rd.damage = damage;
		rd.timestamp = now;
		tstats->recent_damage_taken.push_back(rd);

		// Limpar danos antigos (fora da janela de assist�ncia)
		tstats->recent_damage_taken.erase(
			std::remove_if(tstats->recent_damage_taken.begin(), tstats->recent_damage_taken.end(),
						   [now](const s_arena7x7_recent_damage &d)
						   {
							   return DIFF_TICK(now, d.timestamp) > ARENA7X7_ASSIST_WINDOW;
						   }),
			tstats->recent_damage_taken.end());
	}

	// Atualizar agregado por skill
	uint64 skill_key = ((uint64)attacker_id << 32) | skill_id;
	auto &skill_data = match->damage_by_skill[skill_key];
	if (skill_data.char_id == 0)
	{
		skill_data.char_id = attacker_id;
		skill_data.skill_id = skill_id;
		// Nome da skill ser� preenchido depois, se necess�rio
		skill_data.skill_name = skill_id > 0 ? skill_get_name(skill_id) : "Ataque Normal";
	}
	skill_data.total_damage += damage;
	skill_data.hit_count++;
	if (is_critical)
		skill_data.critical_count++;
	if (damage > skill_data.max_damage)
		skill_data.max_damage = damage;

	// Atualizar agregado por alvo
	uint64 target_key = ((uint64)attacker_id << 32) | target_id;
	auto &target_data = match->damage_by_target[target_key];
	if (target_data.attacker_id == 0)
	{
		target_data.attacker_id = attacker_id;
		target_data.target_id = target_id;
	}
	target_data.total_damage += damage;
	target_data.hit_count++;
}

/**
 * Registra uma kill
 */
void arena7x7_record_kill(std::shared_ptr<s_arena7x7_match> match,
						  uint32 killer_id, uint32 victim_id, uint16 skill_id, uint32 kill_damage)
{

	if (!match || match->status != ARENA7X7_MATCH_ACTIVE)
		return;

	t_tick now = gettick();

	// Buscar assistentes (quem causou dano recente na v�tima)
	std::vector<uint32> assists;
	auto vit = match->players.find(victim_id);
	if (vit != match->players.end() && vit->second)
	{
		auto &vstats = vit->second;

		// Coletar atacantes �nicos (exceto o killer)
		std::unordered_set<uint32> assist_set;
		for (auto &rd : vstats->recent_damage_taken)
		{
			if (rd.attacker_id != killer_id &&
				DIFF_TICK(now, rd.timestamp) <= ARENA7X7_ASSIST_WINDOW)
			{
				assist_set.insert(rd.attacker_id);
			}
		}

		// Converter para vetor (max 3 assists)
		for (uint32 aid : assist_set)
		{
			if (assists.size() >= 3)
				break;
			assists.push_back(aid);
		}

		// Limpar danos recentes ap�s morte
		vstats->recent_damage_taken.clear();

		// Atualizar estat�sticas da v�tima
		vstats->deaths++;
		vstats->current_streak = 0;
		vstats->last_death_time = now;

		// Calcular tempo vivo at� este momento (morte permanente - tumba)
		if (vstats->time_alive == 0)
		{
			vstats->time_alive = (uint32)DIFF_TICK(now, vstats->join_time);
		}
	}

	// Log de kill
	s_arena7x7_kill_entry kill;
	kill.killer_id = killer_id;
	kill.victim_id = victim_id;
	kill.skill_id = skill_id;
	kill.kill_damage = kill_damage;
	kill.assist1_id = assists.size() > 0 ? assists[0] : 0;
	kill.assist2_id = assists.size() > 1 ? assists[1] : 0;
	kill.assist3_id = assists.size() > 2 ? assists[2] : 0;
	kill.timestamp = now;

	// Atualizar estat�sticas do killer
	auto kit = match->players.find(killer_id);
	if (kit != match->players.end() && kit->second)
	{
		auto &kstats = kit->second;
		kstats->kills++;
		kstats->current_streak++;
		if (kstats->current_streak > kstats->best_streak)
		{
			kstats->best_streak = kstats->current_streak;
		}
		kill.killer_streak = kstats->current_streak;
	}

	match->kill_log.push_back(kill);

	// Dar assist�ncias
	for (uint32 assist_id : assists)
	{
		auto ait = match->players.find(assist_id);
		if (ait != match->players.end() && ait->second)
		{
			ait->second->assists++;
		}
	}

	// Atualizar agregado de dano por alvo (marcar kill)
	uint64 target_key = ((uint64)killer_id << 32) | victim_id;
	auto &target_data = match->damage_by_target[target_key];
	target_data.kill_count++;

	// Atualizar agregado de dano por skill (marcar kill)
	uint64 skill_key = ((uint64)killer_id << 32) | skill_id;
	if (match->damage_by_skill.find(skill_key) != match->damage_by_skill.end())
	{
		match->damage_by_skill[skill_key].kill_count++;
	}
}

/**
 * Registra skill de suporte
 */
void arena7x7_record_support(std::shared_ptr<s_arena7x7_match> match,
							 uint32 caster_id, uint32 target_id, uint16 skill_id, e_arena7x7_support_type type, uint32 value)
{

	if (!match || match->status != ARENA7X7_MATCH_ACTIVE)
		return;

	t_tick now = gettick();

	// Log detalhado
	s_arena7x7_support_entry entry;
	entry.caster_id = caster_id;
	entry.target_id = target_id;
	entry.skill_id = skill_id;
	entry.type = type;
	entry.value = value;
	entry.timestamp = now;
	match->support_log.push_back(entry);

	// Atualizar estat�sticas do caster
	auto cit = match->players.find(caster_id);
	if (cit != match->players.end() && cit->second)
	{
		auto &cstats = cit->second;
		cstats->support_skills_used++;

		if (type == ARENA7X7_SUPPORT_HEAL)
		{
			cstats->healing_done += value;
		}
		else if (type == ARENA7X7_SUPPORT_BUFF)
		{
			cstats->buffs_given++;
		}
		else if (type == ARENA7X7_SUPPORT_DEBUFF)
		{
			cstats->debuffs_given++;
		}
	}

	// Atualizar estat�sticas do alvo (se heal)
	if (type == ARENA7X7_SUPPORT_HEAL && target_id != caster_id)
	{
		auto tit = match->players.find(target_id);
		if (tit != match->players.end() && tit->second)
		{
			tit->second->healing_received += value;
		}
	}

	// Atualizar agregado por alvo
	uint64 key = ((uint64)caster_id << 32) | target_id;
	auto &support_data = match->support_by_target[key];
	if (support_data.caster_id == 0)
	{
		support_data.caster_id = caster_id;
		support_data.target_id = target_id;
	}

	if (type == ARENA7X7_SUPPORT_HEAL)
	{
		support_data.total_healing += value;
	}
	if (type == ARENA7X7_SUPPORT_BUFF)
	{
		support_data.buff_count++;
	}
	support_data.skill_count++;
}

/**
 * Registra uso de item
 */
void arena7x7_record_item_use(std::shared_ptr<s_arena7x7_match> match,
							  uint32 char_id, uint32 target_id, t_itemid item_id, const char *item_name,
							  e_arena7x7_item_type type, uint32 value)
{

	if (!match || match->status != ARENA7X7_MATCH_ACTIVE)
		return;

	t_tick now = gettick();

	// Log detalhado
	s_arena7x7_item_entry entry;
	entry.char_id = char_id;
	entry.target_id = target_id;
	entry.item_id = item_id;
	entry.item_name = item_name ? item_name : "";
	entry.type = type;
	entry.value = value;
	entry.timestamp = now;
	match->item_log.push_back(entry);

	// Atualizar estat�sticas do jogador
	auto it = match->players.find(char_id);
	if (it != match->players.end() && it->second)
	{
		auto &stats = it->second;

		switch (type)
		{
		case ARENA7X7_ITEM_HP:
			stats->hp_potions++;
			break;
		case ARENA7X7_ITEM_SP:
			stats->sp_potions++;
			break;
		default:
			stats->other_items++;
			break;
		}

		// Verificar itens espec�ficos pelo ID
		switch (item_id)
		{
		case 715: // Yellow Gemstone
			stats->yellow_gems++;
			break;
		case 716: // Red Gemstone
			stats->red_gems++;
			break;
		case 717: // Blue Gemstone
			stats->blue_gems++;
			break;
		case 678: // Poison Bottle
			stats->poison_bottles++;
			break;
		}
	}
}

/**
 * Registra consumo de SP
 */
void arena7x7_record_sp_use(std::shared_ptr<s_arena7x7_match> match, uint32 char_id, uint32 sp_amount)
{
	if (!match || match->status != ARENA7X7_MATCH_ACTIVE)
		return;

	auto it = match->players.find(char_id);
	if (it != match->players.end() && it->second)
	{
		it->second->sp_consumed += sp_amount;
	}
}

/**
 * Registra consumo de Zeny
 */
void arena7x7_record_zeny_use(std::shared_ptr<s_arena7x7_match> match, uint32 char_id, uint32 zeny_amount)
{
	if (!match || match->status != ARENA7X7_MATCH_ACTIVE)
		return;

	auto it = match->players.find(char_id);
	if (it != match->players.end() && it->second)
	{
		it->second->zeny_consumed += zeny_amount;
	}
}

// ============================================================================
// Fun��es de Persist�ncia SQL
// ============================================================================

/**
 * Salva uma partida completa no banco de dados
 */
bool arena7x7_save_match(std::shared_ptr<s_arena7x7_match> match)
{
	if (!match)
		return false;

	// Calcular dura��o em segundos
	uint32 duration = 0;
	if (match->end_time > match->start_time)
	{
		duration = (uint32)DIFF_TICK(match->end_time, match->start_time) / 1000;
	}

	// Escapar nomes das guilds para SQL
	char esc_blue_guild[NAME_LENGTH * 2 + 1];
	char esc_red_guild[NAME_LENGTH * 2 + 1];
	Sql_EscapeStringLen(mmysql_handle, esc_blue_guild, match->blue_guild_name.c_str(), match->blue_guild_name.length());
	Sql_EscapeStringLen(mmysql_handle, esc_red_guild, match->red_guild_name.c_str(), match->red_guild_name.length());

	// === INICIAR TRANSACAO PARA PERFORMANCE ===
	Sql_Query(mmysql_handle, "START TRANSACTION");

	// Inserir na tabela principal de partidas (agora com guild_id e guild_name)
	if (SQL_ERROR == Sql_Query(mmysql_handle,
							   "INSERT INTO `arena7x7_matches` "
							   "(`match_id`, `season`, `blue_guild_id`, `blue_guild_name`, `red_guild_id`, `red_guild_name`, "
							   "`map_name`, `start_time`, `end_time`, `duration_seconds`, "
							   "`winner_team`, `blue_score`, `red_score`, `status`) "
							   "VALUES (%u, %u, %u, '%s', %u, '%s', '%s', NOW() - INTERVAL %u SECOND, NOW(), %u, %u, %u, %u, %u)",
							   match->match_id, match->season,
							   match->blue_guild_id, esc_blue_guild,
							   match->red_guild_id, esc_red_guild,
							   match->map_name.c_str(),
							   duration, duration,
							   (uint32)match->winner_team, match->blue_score, match->red_score,
							   (uint32)match->status))
	{

		// showerror("arena7x7_save_match: falha ao inserir partida %u\n", match->match_id);
		Sql_Query(mmysql_handle, "ROLLBACK");
		return false;
	}

	// Salvar jogadores
	for (auto &kv : match->players)
	{
		auto &stats = kv.second;
		if (!stats)
			continue;

		bool won = (stats->team == match->winner_team);
		bool lost = (match->winner_team != ARENA7X7_TEAM_NONE && stats->team != match->winner_team);

		// Escapar char_name e guild_name para evitar problemas de encoding
		char esc_char_name[NAME_LENGTH * 2 + 1];
		char esc_guild_name[NAME_LENGTH * 2 + 1];
		Sql_EscapeStringLen(mmysql_handle, esc_char_name, stats->char_name.c_str(), stats->char_name.length());
		Sql_EscapeStringLen(mmysql_handle, esc_guild_name, stats->guild_name.c_str(), stats->guild_name.length());

		// arena7x7_match_players (agora com guild_id e guild_name)
		if (SQL_ERROR == Sql_Query(mmysql_handle,
								   "INSERT INTO `arena7x7_match_players` "
								   "(`match_id`, `char_id`, `account_id`, `char_name`, `team`, `job_class`, "
								   "`base_level`, `guild_id`, `guild_name`, `is_leader`, `is_deserter`, `is_winner`) "
								   "VALUES (%u, %u, %u, '%s', %u, %u, %u, %u, '%s', %d, %d, %d)",
								   match->match_id, stats->char_id, stats->account_id,
								   esc_char_name, (uint32)stats->team, stats->job_class,
								   stats->base_level, stats->guild_id, esc_guild_name,
								   stats->is_leader ? 1 : 0,
								   stats->is_deserter ? 1 : 0, won ? 1 : 0))
		{

			// showwarning("arena7x7_save_match: falha ao inserir jogador %u\n", stats->char_id);
			continue;
		}

		// arena7x7_match_stats
		if (SQL_ERROR == Sql_Query(mmysql_handle,
								   "INSERT INTO `arena7x7_match_stats` "
								   "(`match_id`, `char_id`, `kills`, `deaths`, `assists`, `damage_done`, "
								   "`damage_received`, `healing_done`, `healing_received`, `skills_used`, "
								   "`items_used`, `top_damage`, `best_streak`, `support_skills`, `hp_potions`, "
								   "`sp_potions`, `yellow_gems`, `red_gems`, `blue_gems`, `poison_bottles`, "
								   "`sp_consumed`, `zeny_consumed`, `time_alive`) "
								   "VALUES (%u, %u, %u, %u, %u, %u, %u, %u, %u, %u, %u, %u, %u, %u, %u, %u, %u, %u, %u, %u, %u, %u, %u)",
								   match->match_id, stats->char_id, stats->kills, stats->deaths, stats->assists,
								   stats->damage_done, stats->damage_received, stats->healing_done, stats->healing_received,
								   stats->total_skills_used, stats->total_items_used, stats->top_damage, stats->best_streak,
								   stats->support_skills_used, stats->hp_potions, stats->sp_potions, stats->yellow_gems,
								   stats->red_gems, stats->blue_gems, stats->poison_bottles, stats->sp_consumed, stats->zeny_consumed,
								   stats->time_alive))
		{

			// showwarning("arena7x7_save_match: falha ao inserir stats do jogador %u\n", stats->char_id);
		}

		// Salvar lista detalhada de skills usadas pelo jogador
		for (const auto &skill_kv : stats->skills_used_map)
		{
			uint16 skill_id = skill_kv.first;
			uint32 use_count = skill_kv.second.first;
			const std::string &skill_name = skill_kv.second.second;

			char esc_skill_name[SKILL_NAME_LENGTH * 2 + 1];
			Sql_EscapeStringLen(mmysql_handle, esc_skill_name, skill_name.c_str(), skill_name.length());

			Sql_Query(mmysql_handle,
					  "INSERT INTO `arena7x7_skill_summary` "
					  "(`match_id`, `char_id`, `char_name`, `skill_id`, `skill_name`, `use_count`) "
					  "VALUES (%u, %u, '%s', %u, '%s', %u) "
					  "ON DUPLICATE KEY UPDATE `use_count` = `use_count` + VALUES(`use_count`)",
					  match->match_id, stats->char_id, stats->char_name.c_str(),
					  skill_id, esc_skill_name, use_count);
		}
	}

	// Salvar log de dano detalhado (apenas os primeiros N para n�o sobrecarregar)
	// Usar INSERT em lote para performance
	const size_t MAX_DAMAGE_LOGS = 10000;
	const size_t BATCH_SIZE = 100; // Inserir 100 registros por vez
	size_t count = 0;

	if (!match->damage_log.empty())
	{
		std::string damage_batch;
		size_t batch_count = 0;

		for (auto &entry : match->damage_log)
		{
			if (count++ >= MAX_DAMAGE_LOGS)
				break;

			char value_str[256];
			snprintf(value_str, sizeof(value_str), "(%u, %u, %u, %u, %u, %d, %d)",
					 match->match_id, entry.attacker_id, entry.target_id,
					 entry.skill_id, entry.damage, entry.is_critical ? 1 : 0, entry.is_kill ? 1 : 0);

			if (batch_count == 0)
			{
				damage_batch = "INSERT INTO `arena7x7_damage_log` "
							   "(`match_id`, `attacker_id`, `target_id`, `skill_id`, `damage`, "
							   "`is_critical`, `is_kill`) VALUES ";
				damage_batch += value_str;
			}
			else
			{
				damage_batch += ",";
				damage_batch += value_str;
			}
			batch_count++;

			// Executar batch quando atingir o limite ou for o ultimo
			if (batch_count >= BATCH_SIZE || count >= MAX_DAMAGE_LOGS || count >= match->damage_log.size())
			{
				Sql_QueryStr(mmysql_handle, damage_batch.c_str());
				batch_count = 0;
				damage_batch.clear();
			}
		}
	}

	// Salvar dano agregado por skill - usar INSERT em lote
	if (!match->damage_by_skill.empty())
	{
		std::string skill_batch = "INSERT INTO `arena7x7_damage_by_skill` "
								  "(`match_id`, `char_id`, `skill_id`, `skill_name`, `total_damage`, "
								  "`hit_count`, `critical_count`, `kill_count`, `max_damage`) VALUES ";
		bool first = true;
		for (auto &kv : match->damage_by_skill)
		{
			auto &data = kv.second;
			char esc_skill_name[SKILL_NAME_LENGTH * 2 + 1];
			Sql_EscapeStringLen(mmysql_handle, esc_skill_name, data.skill_name.c_str(), data.skill_name.length());

			char value_str[512];
			snprintf(value_str, sizeof(value_str), "%s(%u, %u, %u, '%s', %u, %u, %u, %u, %u)",
					 first ? "" : ",", match->match_id, data.char_id, data.skill_id, esc_skill_name,
					 data.total_damage, data.hit_count, data.critical_count, data.kill_count, data.max_damage);
			skill_batch += value_str;
			first = false;
		}
		Sql_QueryStr(mmysql_handle, skill_batch.c_str());
	}

	// Salvar dano agregado por alvo - usar INSERT em lote
	if (!match->damage_by_target.empty())
	{
		std::string target_batch = "INSERT INTO `arena7x7_damage_by_target` "
								   "(`match_id`, `attacker_id`, `target_id`, `total_damage`, `hit_count`, `kill_count`) VALUES ";
		bool first = true;
		for (auto &kv : match->damage_by_target)
		{
			auto &data = kv.second;
			char value_str[256];
			snprintf(value_str, sizeof(value_str), "%s(%u, %u, %u, %u, %u, %u)",
					 first ? "" : ",", match->match_id, data.attacker_id, data.target_id,
					 data.total_damage, data.hit_count, data.kill_count);
			target_batch += value_str;
			first = false;
		}
		Sql_QueryStr(mmysql_handle, target_batch.c_str());
	}

	// Salvar log de suporte - usar INSERT em lote
	if (!match->support_log.empty())
	{
		std::string support_batch = "INSERT INTO `arena7x7_support_log` "
									"(`match_id`, `caster_id`, `target_id`, `skill_id`, `support_type`, `value`) VALUES ";
		bool first = true;
		size_t batch_count = 0;
		for (auto &entry : match->support_log)
		{
			char value_str[256];
			snprintf(value_str, sizeof(value_str), "%s(%u, %u, %u, %u, %u, %u)",
					 first ? "" : ",", match->match_id, entry.caster_id, entry.target_id,
					 entry.skill_id, (uint32)entry.type, entry.value);
			support_batch += value_str;
			first = false;
			batch_count++;

			if (batch_count >= 100)
			{
				Sql_QueryStr(mmysql_handle, support_batch.c_str());
				support_batch = "INSERT INTO `arena7x7_support_log` "
								"(`match_id`, `caster_id`, `target_id`, `skill_id`, `support_type`, `value`) VALUES ";
				first = true;
				batch_count = 0;
			}
		}
		if (!first) // Se ainda tem dados no batch
			Sql_QueryStr(mmysql_handle, support_batch.c_str());
	}

	// Salvar suporte por alvo - usar INSERT em lote
	if (!match->support_by_target.empty())
	{
		std::string support_target_batch = "INSERT INTO `arena7x7_support_by_target` "
										   "(`match_id`, `caster_id`, `target_id`, `total_healing`, `buff_count`, `skill_count`) VALUES ";
		bool first = true;
		for (auto &kv : match->support_by_target)
		{
			auto &data = kv.second;
			char value_str[256];
			snprintf(value_str, sizeof(value_str), "%s(%u, %u, %u, %u, %u, %u)",
					 first ? "" : ",", match->match_id, data.caster_id, data.target_id,
					 data.total_healing, data.buff_count, data.skill_count);
			support_target_batch += value_str;
			first = false;
		}
		Sql_QueryStr(mmysql_handle, support_target_batch.c_str());
	}

	// Salvar log de itens - usar INSERT em lote
	if (!match->item_log.empty())
	{
		std::string item_batch = "INSERT INTO `arena7x7_item_usage` "
								 "(`match_id`, `char_id`, `target_id`, `item_id`, `item_name`, `item_type`, `value`) VALUES ";
		bool first = true;
		size_t batch_count = 0;
		for (auto &entry : match->item_log)
		{
			char esc_item_name[100 * 2 + 1];
			Sql_EscapeStringLen(mmysql_handle, esc_item_name, entry.item_name.c_str(), entry.item_name.length());

			char value_str[512];
			snprintf(value_str, sizeof(value_str), "%s(%u, %u, %u, %u, '%s', %u, %u)",
					 first ? "" : ",", match->match_id, entry.char_id, entry.target_id,
					 entry.item_id, esc_item_name, (uint32)entry.type, entry.value);
			item_batch += value_str;
			first = false;
			batch_count++;

			if (batch_count >= 100)
			{
				Sql_QueryStr(mmysql_handle, item_batch.c_str());
				item_batch = "INSERT INTO `arena7x7_item_usage` "
							 "(`match_id`, `char_id`, `target_id`, `item_id`, `item_name`, `item_type`, `value`) VALUES ";
				first = true;
				batch_count = 0;
			}
		}
		if (!first)
			Sql_QueryStr(mmysql_handle, item_batch.c_str());
	}

	// Salvar log de kills - usar INSERT em lote
	if (!match->kill_log.empty())
	{
		std::string kill_batch = "INSERT INTO `arena7x7_kills` "
								 "(`match_id`, `killer_id`, `victim_id`, `kill_skill_id`, `kill_damage`, `assist1_id`, "
								 "`assist2_id`, `assist3_id`, `killer_streak`) VALUES ";
		bool first = true;
		for (auto &entry : match->kill_log)
		{
			char value_str[256];
			snprintf(value_str, sizeof(value_str), "%s(%u, %u, %u, %u, %u, %u, %u, %u, %u)",
					 first ? "" : ",", match->match_id, entry.killer_id, entry.victim_id,
					 entry.skill_id, entry.kill_damage, entry.assist1_id, entry.assist2_id, entry.assist3_id, entry.killer_streak);
			kill_batch += value_str;
			first = false;
		}
		Sql_QueryStr(mmysql_handle, kill_batch.c_str());
	}

	// === FINALIZAR TRANSACAO ===
	Sql_Query(mmysql_handle, "COMMIT");

	// #if ARENA7X7_DEBUG
	// 	ShowInfo("Arena7x7: Partida %u salva no banco de dados\n", match->match_id);
	// #endif
	return true;
}

/**
 * Atualiza o ranking acumulado de um jogador
 * @param count_match - Se false, n�o incrementa contador de partidas (usado para desertores de partidas canceladas)
 */
bool arena7x7_update_ranking(uint32 char_id, s_arena7x7_player_stats *stats, bool won, bool lost, bool tie, bool count_match)
{
	if (!stats)
		return false;

	// Calcular pontos
	int points = 0;

	// Se o jogador � desertor, aplica penalidade de -3 pontos e +1 death
	if (stats->is_deserter)
	{
		points = -3;
		stats->deaths += 1; // Contar como morte
							// #if ARENA7X7_DEBUG
							// 		ShowInfo("Arena7x7: Jogador %s marcado como desertor - penalidade aplicada (-3 pontos, +1 death)\n", stats->char_name.c_str());
							// #endif
	}
	else if (won)
		points = 3;
	else if (lost)
		points = 0;
	else
		points = 1; // Empate

	// Verificar se jogador j� existe no ranking
	if (SQL_ERROR == Sql_Query(mmysql_handle,
							   "SELECT `char_id` FROM `arena7x7_ranking` WHERE `char_id` = %u",
							   char_id))
	{
		return false;
	}

	if (Sql_NumRows(mmysql_handle) > 0)
	{
		// Update existente
		Sql_FreeResult(mmysql_handle);

		if (SQL_ERROR == Sql_Query(mmysql_handle,
								   "UPDATE `arena7x7_ranking` SET "
								   "`wins` = `wins` + %d, "
								   "`losses` = `losses` + %d, "
								   "`ties` = `ties` + %d, "
								   "`matches_played` = `matches_played` + %d, "
								   "`total_kills` = `total_kills` + %u, "
								   "`total_deaths` = `total_deaths` + %u, "
								   "`total_assists` = `total_assists` + %u, "
								   "`total_damage_done` = `total_damage_done` + %u, "
								   "`total_damage_received` = `total_damage_received` + %u, "
								   "`total_healing_done` = `total_healing_done` + %u, "
								   "`total_skills_used` = `total_skills_used` + %u, "
								   "`total_items_used` = `total_items_used` + %u, "
								   "`points` = GREATEST(0, CAST(`points` AS SIGNED) + %d), "
								   "`season_points` = GREATEST(0, CAST(`season_points` AS SIGNED) + %d), "
								   "`best_damage_match` = GREATEST(`best_damage_match`, %u), "
								   "`best_killstreak` = GREATEST(`best_killstreak`, %u), "
								   "`best_kills_match` = GREATEST(`best_kills_match`, %u), "
								   "`last_match` = NOW() "
								   "WHERE `char_id` = %u",
								   won ? 1 : 0, lost ? 1 : 0, tie ? 1 : 0, count_match ? 1 : 0,
								   stats->kills, stats->deaths, stats->assists,
								   stats->damage_done, stats->damage_received, stats->healing_done,
								   stats->total_skills_used, stats->total_items_used,
								   points, points, stats->damage_done, stats->best_streak, stats->kills,
								   char_id))
		{
			return false;
		}
	}
	else
	{
		// Insert novo
		Sql_FreeResult(mmysql_handle);

		// Escapar char_name para evitar problemas de encoding
		char esc_char_name[NAME_LENGTH * 2 + 1];
		Sql_EscapeStringLen(mmysql_handle, esc_char_name, stats->char_name.c_str(), stats->char_name.length());

		if (SQL_ERROR == Sql_Query(mmysql_handle,
								   "INSERT INTO `arena7x7_ranking` "
								   "(`char_id`, `char_name`, `account_id`, `wins`, `losses`, `ties`, `matches_played`, "
								   "`total_kills`, `total_deaths`, `total_assists`, `total_damage_done`, "
								   "`total_damage_received`, `total_healing_done`, `total_skills_used`, `total_items_used`, "
								   "`points`, `season_points`, `best_damage_match`, `best_killstreak`, `best_kills_match`, `last_match`) "
								   "VALUES (%u, '%s', %u, %d, %d, %d, %d, %u, %u, %u, %u, %u, %u, %u, %u, %d, %d, %u, %u, %u, NOW())",
								   char_id, esc_char_name, stats->account_id,
								   won ? 1 : 0, lost ? 1 : 0, tie ? 1 : 0, count_match ? 1 : 0,
								   stats->kills, stats->deaths, stats->assists,
								   stats->damage_done, stats->damage_received, stats->healing_done,
								   stats->total_skills_used, stats->total_items_used,
								   points > 0 ? points : 0, points > 0 ? points : 0,
								   stats->damage_done, stats->best_streak, stats->kills))
		{
			return false;
		}
	}

	// Atualizar ranking por classe tamb�m
	if (SQL_ERROR == Sql_Query(mmysql_handle,
							   "SELECT `char_id` FROM `arena7x7_ranking_by_class` WHERE `char_id` = %u AND `class` = %u",
							   char_id, stats->job_class))
	{
		return true; // N�o � cr�tico
	}

	if (Sql_NumRows(mmysql_handle) > 0)
	{
		Sql_FreeResult(mmysql_handle);

		Sql_Query(mmysql_handle,
				  "UPDATE `arena7x7_ranking_by_class` SET "
				  "`matches_played` = `matches_played` + %d, "
				  "`wins` = `wins` + %d, "
				  "`total_kills` = `total_kills` + %u, "
				  "`total_deaths` = `total_deaths` + %u, "
				  "`total_damage` = `total_damage` + %u, "
				  "`total_healing` = `total_healing` + %u "
				  "WHERE `char_id` = %u AND `class` = %u",
				  count_match ? 1 : 0, won ? 1 : 0,
				  stats->kills, stats->deaths,
				  stats->damage_done, stats->healing_done,
				  char_id, stats->job_class);
	}
	else
	{
		Sql_FreeResult(mmysql_handle);

		Sql_Query(mmysql_handle,
				  "INSERT INTO `arena7x7_ranking_by_class` "
				  "(`char_id`, `class`, `matches_played`, `wins`, `total_kills`, `total_deaths`, `total_damage`, `total_healing`) "
				  "VALUES (%u, %u, %d, %d, %u, %u, %u, %u)",
				  char_id, stats->job_class,
				  count_match ? 1 : 0, won ? 1 : 0,
				  stats->kills, stats->deaths,
				  stats->damage_done, stats->healing_done);
	}

	return true;
}

/**
 * Carrega o pr�ximo match_id do banco
 */
uint32 arena7x7_load_next_match_id()
{
	if (SQL_ERROR == Sql_Query(mmysql_handle,
							   "SELECT COALESCE(MAX(`match_id`), 0) + 1 FROM `arena7x7_matches`"))
	{
		// showerror("arena7x7_load_next_match_id: falha na query\n");
		return 1;
	}

	char *data;
	if (SQL_SUCCESS == Sql_NextRow(mmysql_handle))
	{
		Sql_GetData(mmysql_handle, 0, &data, nullptr);
		uint32 next_id = (uint32)strtoul(data, nullptr, 10);
		Sql_FreeResult(mmysql_handle);
		return next_id;
	}

	Sql_FreeResult(mmysql_handle);
	return 1;
}

/**
 * Carrega a temporada atual
 */
uint16 arena7x7_get_current_season()
{
	if (SQL_ERROR == Sql_Query(mmysql_handle,
							   "SELECT `season_id` FROM `arena7x7_seasons` WHERE `is_active` = 1 ORDER BY `season_id` DESC LIMIT 1"))
	{
		return 1;
	}

	char *data;
	if (SQL_SUCCESS == Sql_NextRow(mmysql_handle))
	{
		Sql_GetData(mmysql_handle, 0, &data, nullptr);
		uint16 season = (uint16)strtoul(data, nullptr, 10);
		Sql_FreeResult(mmysql_handle);
		return season > 0 ? season : 1;
	}

	Sql_FreeResult(mmysql_handle);
	return 1;
}

// ============================================================================
// Fun��es de Utilidade
// ============================================================================

/**
 * Verifica se um jogador est� em uma partida ativa
 */
bool arena7x7_is_player_in_match(uint32 char_id)
{
	auto match = arena7x7_get_player_match(char_id);
	return match && (match->status == ARENA7X7_MATCH_WAITING || match->status == ARENA7X7_MATCH_ACTIVE);
}

/**
 * Obt�m o time de um jogador
 */
e_arena7x7_team arena7x7_get_player_team(uint32 char_id)
{
	auto match = arena7x7_get_player_match(char_id);
	if (!match)
		return ARENA7X7_TEAM_NONE;

	auto it = match->players.find(char_id);
	if (it != match->players.end() && it->second)
	{
		return it->second->team;
	}
	return ARENA7X7_TEAM_NONE;
}

/**
 * Verifica se dois jogadores s�o do mesmo time
 */
bool arena7x7_same_team(uint32 char_id1, uint32 char_id2)
{
	auto team1 = arena7x7_get_player_team(char_id1);
	auto team2 = arena7x7_get_player_team(char_id2);

	return team1 != ARENA7X7_TEAM_NONE && team1 == team2;
}

/**
 * Conta jogadores vivos em cada time
 */
void arena7x7_count_alive_players(std::shared_ptr<s_arena7x7_match> match)
{
	if (!match)
		return;

	match->blue_alive = 0;
	match->red_alive = 0;

	for (auto &kv : match->players)
	{
		auto &stats = kv.second;
		if (!stats || stats->is_dead)
			continue;

		if (stats->team == ARENA7X7_TEAM_BLUE)
		{
			match->blue_alive++;
		}
		else if (stats->team == ARENA7X7_TEAM_RED)
		{
			match->red_alive++;
		}
	}
}

/**
 * Atualiza o placar do BG (contador de jogadores vivos)
 */
void arena7x7_update_score_display(std::shared_ptr<s_arena7x7_match> match)
{
	if (!match || match->map_name.empty())
		return;

	int16 m = map_mapname2mapid(match->map_name.c_str());
	if (m < 0)
		return;

	struct map_data *mapdata = map_getmapdata(m);
	if (!mapdata)
		return;

	// Atualizar contagem de jogadores vivos
	arena7x7_count_alive_players(match);

	// Usar bgscore_lion para Time Azul e bgscore_eagle para Time Vermelho
	mapdata->bgscore_lion = match->blue_alive;
	mapdata->bgscore_eagle = match->red_alive;

	// Enviar atualiza��o para todos os jogadores no mapa via protocolo BG
	clif_bg_updatescore(m);

	// #if ARENA7X7_DEBUG
	// 	ShowInfo("Arena7x7: Placar atualizado - %s: %d vs %s: %d\n",
	// 			 match->blue_guild_name.c_str(), match->blue_alive,
	// 			 match->red_guild_name.c_str(), match->red_alive);
	// #endif
}

// in�cio do custom moskaum
/**
 * Callback para remover SC_CURSEDCIRCLE_TARGET dos alvos
 */
static int arena7x7_remove_cursedcircle_callback(struct block_list *bl, va_list ap)
{
	uint32 shura_id = va_arg(ap, uint32);

	if (bl->type != BL_PC)
		return 0;

	map_session_data *target = (map_session_data *)bl;
	status_change *tsc = status_get_sc(bl);

	if (tsc && tsc->getSCE(SC_CURSEDCIRCLE_TARGET))
	{
		// Verificar se este alvo est� preso pelo Shura que morreu
		if (tsc->getSCE(SC_CURSEDCIRCLE_TARGET)->val2 == (int)shura_id)
		{
			// #if ARENA7X7_DEBUG
			// 			ShowInfo("Arena7x7: Removendo SC_CURSEDCIRCLE_TARGET de %s\n", target->status.name);
			// #endif
			// Enviar packet de fim do bladestop visual
			clif_bladestop(*bl, shura_id, false);
			// Remover o status
			status_change_end(bl, SC_CURSEDCIRCLE_TARGET);
		}
	}
	return 0;
}

/**
 * Remove SC_CURSEDCIRCLE_TARGET de todos os alvos quando o Shura morre
 * Chamado antes de transformar em tumba
 */
static void arena7x7_remove_cursedcircle_targets(map_session_data *sd)
{
	nullpo_retv(sd);

	// Verificar se o jogador tem SC_CURSEDCIRCLE_ATKER ativo
	status_change *sc = status_get_sc((struct block_list *)sd);
	if (!sc || !sc->getSCE(SC_CURSEDCIRCLE_ATKER))
		return; // N�o tem Cursed Circle ativo

	uint32 shura_id = sd->id;

	// #if ARENA7X7_DEBUG
	// 	ShowInfo("Arena7x7: Removendo SC_CURSEDCIRCLE_TARGET de alvos do Shura %s (ID: %u)\n",
	// 		sd->status.name, shura_id);
	// #endif

	// Procurar em toda a �rea do mapa
	map_foreachinmap(arena7x7_remove_cursedcircle_callback, sd->m, BL_PC, shura_id);

	// Remover o status do pr�prio Shura tamb�m
	status_change_end((struct block_list *)sd, SC_CURSEDCIRCLE_ATKER);
}
// fim do custom
/**
 * Transforma jogador em tumba (morte permanente na arena)
 * O jogador fica MORTO e n�o pode ser ressuscitado at� o fim da partida
 */
/**
 * Transforma jogador em tumba (morte permanente na arena)
 * O jogador fica MORTO e n�o pode ser ressuscitado at� o fim da partida.
 * Usa disguise para mostrar visualmente uma tumba de MVP.
 */
void arena7x7_transform_to_tombstone(map_session_data *sd)
{
	nullpo_retv(sd);

	// #if ARENA7X7_DEBUG
	// 	ShowInfo("Arena7x7 DEBUG: === INICIO transform_to_tombstone para %s ===\n", sd->status.name);
	// 	ShowInfo("Arena7x7 DEBUG: HP atual: %d/%d, dead_sit: %d, vd.dead_sit: %d\n",
	// 			 sd->battle_status.hp, sd->battle_status.max_hp, sd->state.dead_sit, sd->vd.dead_sit);
	// #endif

	auto stats = arena7x7_get_player_stats(sd->status.char_id);
	if (!stats)
	{
		// showwarning("Arena7x7: transform_to_tombstone - stats nao encontrado para %s\n", sd->status.name);
		return;
	}

	// 	// OFICIAL: Player fica MORTO mas com disguise vis�vel
	// 	// N�o reviver! O disguise funciona com player morto

	// 	// Garantir que est� no estado morto correto
	// 	if (!pc_isdead(sd)) {
	// 		sd->state.dead_sit = 1; // Morto deitado
	// 		sd->vd.dead_sit = 1;
	// 	}

	// // #if ARENA7X7_DEBUG
	// // 	ShowInfo("Arena7x7 DEBUG: Player permanece morto (dead_sit=%d)\n", sd->state.dead_sit);
	// // #endif

	// 	// AGORA marcar como morto no sistema Arena7x7 (impede ressurrei��o e dano futuros)
	// 	// Fazer isso ANTES do disguise para que a prote��o contra dano j� esteja ativa
	// 	stats->is_dead = true;
	// // #if ARENA7X7_DEBUG
	// // 	ShowInfo("Arena7x7 DEBUG: Marcado is_dead=true\n");
	// // 	ShowInfo("Arena7x7 DEBUG: Aplicando pc_disguise(MOBID_ARENA_TOMBSTONE = %d)...\n", MOBID_ARENA_TOMBSTONE);
	// // #endif
	// 	// Transformar visualmente em tumba arena (mob ID 22301 - ARENA_TOMBSTONE)
	// 	// IMPORTANTE: Mob 22301 usa sprite customizado para evitar cursor de conversa
	// 	// pc_disguise(sd, MOBID_ARENA_TOMBSTONE);

	// // #if ARENA7X7_DEBUG
	// // 	ShowInfo("Arena7x7 DEBUG: Apos pc_disguise - disguise: %d\n", sd->disguise);
	// // #endif

	// 	// NAO chamar clif_spawn! O pc_disguise ja envia os packets necessarios
	// 	// Players mortos com disguise aparecem normalmente no cliente

	// 	// Parar movimento e ataques
	// 	unit_stop_walking(sd, USW_FIXPOS);
	// 	unit_stop_attack(sd);

	// 	// Aplicar SC_STOP para imobilizar sem efeito visual (30 minutos)
	// // #if ARENA7X7_DEBUG
	// // 	ShowInfo("Arena7x7 DEBUG: Aplicando SC_STOP...\n");
	// // #endif
	// 	status_change_start(NULL, sd, SC_STOP, 10000, 0, 0, 0, 0, 1800000, SCSTART_NOAVOID | SCSTART_NOTICKDEF | SCSTART_NORATEDEF, 0);
	// Marcar como morto no sistema Arena7x7 (impede ressurrei��o e dano futuros)
	stats->is_dead = true;

#if ARENA7X7_DEBUG
	ShowInfo("Arena7x7 DEBUG: transform_to_tombstone - %s marcado como is_dead=true\n", sd->status.name);
#endif

	// IMPORTANTE: Cancelar Devotion ANTES de qualquer outra coisa
	// Isso remove o efeito visual e impede que o jogador morto continue protegendo outros
	status_change_end(sd, SC_DEVOTION);

	// IMPORTANTE: Cancelar status de invisibilidade ANTES de mostrar como morto
	// Isso corrige o bug onde Feint Bomb impedia o jogador de aparecer deitado
	status_change_end(sd, SC__FEINTBOMB);
	status_change_end(sd, SC_CLOAKING);
	status_change_end(sd, SC_CLOAKINGEXCEED);
	status_change_end(sd, SC_CHASEWALK);
	status_change_end(sd, SC_CAMOUFLAGE);
	status_change_end(sd, SC_STEALTHFIELD);
	status_change_end(sd, SC_SUHIDE);
	status_change_end(sd, SC__INVISIBILITY);

	// // IMPORTANTE: Remover TODOS os status/buffs do jogador ao morrer na arena
	// // Isso é mais limpo e evita comportamentos estranhos (jogador morto com buffs)
	// status_change_clear(sd, 3); // 3 = clear all (equivalente a morte)

	// Remover SC_CURSEDCIRCLE_TARGET explicitamente ANTES do clear
	// pois NoClearbuff impede que status_change_clear(3) o remova,
	// o que deixaria o Shura preso com SC_CURSEDCIRCLE_ATKER indefinidamente
	status_change_end(sd, SC_CURSEDCIRCLE_TARGET);

	// IMPORTANTE: Remover TODOS os status/buffs do jogador ao morrer na arena
	status_change_clear(sd, 3); // 3 = clear all (equivalente a morte)

	// IMPORTANTE: Garantir que jogador est� MORTO e DEITADO
	// Setar HP para 0 para garantir estado de morte
	// sd->status.hp = 0;
	// sd->battle_status.hp = 0;

	// For�ar estado visual de morto (deitado)
	// sd->state.dead_sit = 1;
	// sd->vd.dead_sit = 1;

	// Parar qualquer movimento/a��o
	// unit_stop_walking(sd, USW_FIXPOS);
	unit_stop_attack(sd);

	// Cancelar qualquer cast em andamento
	unit_skillcastcancel(sd, 0);

	// Enviar packet para for�ar cliente mostrar jogador morto (deitado)
	clif_clearunit_area(*sd, CLR_DEAD);
	clif_spawn(sd);

	// Aplicar SC_STOP para imobilizar completamente
	// status_change_start(NULL, sd, SC_STOP, 10000, 0, 0, 0, 0, 1800000,
	// 	SCSTART_NOAVOID | SCSTART_NOTICKDEF | SCSTART_NORATEDEF, 0);

	// IMPORTANTE: Cancelar respawn timer se foi adicionado por pc_dead()
	// O pc_dead() � chamado ANTES de arena7x7_on_death em status_damage()
	if (sd->respawn_tid != INVALID_TIMER)
	{
		// #if ARENA7X7_DEBUG
		// 		ShowInfo("Arena7x7 DEBUG: Cancelando respawn timer de %s (tid: %d)\n", sd->status.name, sd->respawn_tid);
		// #endif
		delete_timer(sd->respawn_tid, NULL);
		sd->respawn_tid = INVALID_TIMER;
		;
	}

	// NAO remover do BG aqui! Isso impede que pc_dead() detecte arena7x7 na proxima morte
	// A verificacao de is_dead em bg_warp vai impedir que tumbas sejam teleportadas

	// Notificar o jogador
	clif_displaymessage(sd->fd, "[Arena 7x7] Voce foi eliminado! Aguarde o fim da partida.");

	// #if ARENA7X7_DEBUG
	// 	ShowInfo("Arena7x7 DEBUG: === FIM transform_to_tombstone para %s ===\n", sd->status.name);
	// #endif

	// Atualizar placar
	auto match = arena7x7_get_player_match(sd->status.char_id);
	if (match)
	{
		arena7x7_update_score_display(match);

		// Verificar se algum time foi eliminado (0 jogadores vivos)
		if (match->blue_alive == 0 || match->red_alive == 0)
		{
			// Um time foi eliminado - a partida deve ser finalizada pelo NPC
			// #if ARENA7X7_DEBUG
			// 			ShowInfo("Arena7x7: Time %s foi eliminado!\n",
			// 					 match->blue_alive == 0 ? match->blue_guild_name.c_str() : match->red_guild_name.c_str());
			// #endif
		}
	}
}

/**
 * Remove status de tumba e restaura jogador
 * Chamado no fim da partida para reviver todos os mortos
 */
/**
 * Remove status de tumba e restaura jogador ao normal.
 * Chamado no fim da partida para reviver todos os mortos.
 * Restaura HP/SP para 100% e remove o disguise de tumba.
 */
void arena7x7_restore_from_tombstone(map_session_data *sd)
{
	nullpo_retv(sd);

	// #if ARENA7X7_DEBUG
	// 	ShowInfo("Arena7x7: restore_from_tombstone chamado para %s\n", sd->status.name);
	// #endif

	auto stats = arena7x7_get_player_stats(sd->status.char_id);
	if (stats)
	{
		stats->is_dead = false; // Permite a��es novamente
	}

	// Remover status de imobiliza��o
	status_change_end(sd, SC_STOP);
	status_change_end(sd, SC_STONE);
	status_change_end(sd, SC_ANKLE);
	status_change_end(sd, SC_SILENCE);

	// Remover disguise de tumba (volta ao sprite original)
	pc_disguise(sd, 0);

	// Restaurar HP/SP totais
	status_percent_heal(sd, 100, 100);

	// #if ARENA7X7_DEBUG
	// 	ShowInfo("Arena7x7: %s restaurado com sucesso\n", sd->status.name);
	// #endif
	clif_displaymessage(sd->fd, "[Arena 7x7] Voce foi restaurado.");
}

/**
 * Restaura todos os t�mulos de uma partida
 * Chamado no fim da partida para restaurar todos os jogadores mortos
 */
void arena7x7_restore_all_tombstones(uint32 match_id)
{
	auto match = arena7x7_get_match(match_id);
	if (!match)
	{
		// showwarning("Arena7x7: restore_all_tombstones - partida %u nao encontrada\n", match_id);
		return;
	}

	// #if ARENA7X7_DEBUG
	// 	ShowInfo("Arena7x7: Restaurando todos os tumbas da partida %u\n", match_id);
	// #endif

	// Iterar sobre todos os jogadores da partida
	for (const auto &kv : match->players)
	{
		uint32 char_id = kv.first;
		auto stats = kv.second;

		// Verificar se o jogador est� morto (tumba)
		if (stats && stats->is_dead)
		{
			// Encontrar o jogador online
			map_session_data *sd = map_charid2sd(char_id);
			if (sd)
			{
				// #if ARENA7X7_DEBUG
				// 				ShowInfo("Arena7x7: Restaurando tumba %s (char_id: %u)\n", sd->status.name, char_id);
				// #endif
				arena7x7_restore_from_tombstone(sd);
			}
			else
			{
				// #if ARENA7X7_DEBUG
				// 				ShowInfo("Arena7x7: Jogador %u nao esta online para restaurar\n", char_id);
				// #endif
			}
		}
	}

	// #if ARENA7X7_DEBUG
	// 	ShowInfo("Arena7x7: Restauracao de tumbas da partida %u concluida\n", match_id);
	// #endif
}

/**
 * Obt�m estat�sticas de um jogador na partida atual
 */
std::shared_ptr<s_arena7x7_player_stats> arena7x7_get_player_stats(uint32 char_id)
{
	auto match = arena7x7_get_player_match(char_id);
	if (!match)
		return nullptr;

	auto it = match->players.find(char_id);
	if (it != match->players.end())
	{
		return it->second;
	}
	return nullptr;
}

// ============================================================================
// Hooks para integra��o com o c�digo existente
// ============================================================================

/**
 * Hook chamado quando ocorre dano
 */
void arena7x7_on_damage(struct block_list *src, struct block_list *target,
						uint16 skill_id, int damage, bool is_critical)
{

	if (!src || !target || damage <= 0)
		return;

	// Alvo deve ser jogador
	if (target->type != BL_PC)
		return;

	// Obter o jogador de origem do dano
	map_session_data *src_sd = nullptr;
	if (src->type == BL_PC)
	{
		src_sd = (map_session_data *)src;
	}
	else
	{
		// Se for homunculus, elemental, mercenary, etc - buscar o master
		block_list *master = battle_get_master(src);
		if (master && master->type == BL_PC)
		{
			src_sd = (map_session_data *)master;
		}
	}

	if (!src_sd)
		return;

	map_session_data *target_sd = (map_session_data *)target;

	// Verificar se ambos est�o na mesma partida
	auto match = arena7x7_get_player_match(src_sd->status.char_id);
	if (!match)
		return;

	auto target_match = arena7x7_get_player_match(target_sd->status.char_id);
	if (!target_match || target_match->match_id != match->match_id)
		return;

	// Registrar dano
	arena7x7_record_damage(match, src_sd->status.char_id, target_sd->status.char_id,
						   skill_id, (uint32)damage, is_critical);
}

/**
 * Hook chamado quando um jogador morre
 */
/**
 * Hook chamado quando um jogador morre na arena.
 * Registra a kill/morte nas estat�sticas e transforma o jogador em tumba.
 */
void arena7x7_on_death(map_session_data *killer, map_session_data *victim, uint16 skill_id)
{
	if (!victim)
	{
		// showwarning("Arena7x7: on_death chamado com victim NULL\n");
		return;
	}

	// #if ARENA7X7_DEBUG
	// 	ShowInfo("Arena7x7: on_death chamado - victim=%s, killer=%s\n",
	// 			 victim->status.name,
	// 			 killer ? killer->status.name : "NULL");
	// #endif

	auto match = arena7x7_get_player_match(victim->status.char_id);
	if (!match)
	{
#if ARENA7X7_DEBUG
		// showwarning("Arena7x7: on_death - %s nao esta em uma partida\n", victim->status.name);
#endif
		return;
	}

	// Verificar se o jogador j� est� morto (tumba) - evita processar duas vezes
	auto vstats = arena7x7_get_player_stats(victim->status.char_id);
	if (vstats && vstats->is_dead)
	{
		// #if ARENA7X7_DEBUG
		// 		ShowInfo("Arena7x7: %s ja esta morto (tumba), ignorando\n", victim->status.name);
		// #endif
		return;
	}

	uint32 killer_id = killer ? killer->status.char_id : 0;
	uint16 final_skill_id = skill_id; // skill_id=0 � v�lido (ataque normal)
	uint32 final_kill_damage = 0;

	// Se killer n�o est� na partida, n�o registrar como kill normal
	if (killer)
	{
		auto killer_match = arena7x7_get_player_match(killer->status.char_id);
		if (!killer_match || killer_match->match_id != match->match_id)
		{
			killer_id = 0;
		}
	}

	// Se n�o temos killer direto, tentar descobrir pelo �ltimo dano recebido
	if (killer_id == 0)
	{
		auto vit = match->players.find(victim->status.char_id);
		if (vit != match->players.end() && vit->second && !vit->second->recent_damage_taken.empty())
		{
			// Buscar o atacante mais recente que est� na partida
			t_tick now = gettick();
			t_tick most_recent = 0;

			for (auto &rd : vit->second->recent_damage_taken)
			{
				// Verificar se o dano foi recente (dentro da janela de assist)
				if (DIFF_TICK(now, rd.timestamp) <= ARENA7X7_ASSIST_WINDOW)
				{
					// Verificar se este atacante est� na partida
					auto attacker_match = arena7x7_get_player_match(rd.attacker_id);
					if (attacker_match && attacker_match->match_id == match->match_id)
					{
						if (rd.timestamp > most_recent)
						{
							most_recent = rd.timestamp;
							killer_id = rd.attacker_id;
							final_skill_id = rd.skill_id;  // skill_id=0 � v�lido (ataque normal)
							final_kill_damage = rd.damage; // Dano do �ltimo hit
						}
					}
				}
			}
#if ARENA7X7_DEBUG
			if (killer_id > 0)
			{
				ShowInfo("Arena7x7 DEBUG: Killer encontrado via recent_damage: char_id=%u, skill_id=%u, damage=%u\n", killer_id, final_skill_id, final_kill_damage);
			}
#endif
		}
	}
	else
	{
		// Se temos killer direto, buscar o dano final no recent_damage
		auto vit = match->players.find(victim->status.char_id);
		if (vit != match->players.end() && vit->second && !vit->second->recent_damage_taken.empty())
		{
			// Buscar o �ltimo dano deste killer espec�fico
			for (auto it = vit->second->recent_damage_taken.rbegin(); it != vit->second->recent_damage_taken.rend(); ++it)
			{
				if (it->attacker_id == killer_id)
				{
					final_kill_damage = it->damage;
					if (skill_id == 0) // Se n�o temos skill_id direto, usar do registro
						final_skill_id = it->skill_id;
					break;
				}
			}
		}
	}

#if ARENA7X7_DEBUG
	ShowInfo("Arena7x7 DEBUG: on_death - victim=%s, killer_id=%u, final_skill_id=%u, kill_damage=%u\n",
			 victim->status.name, killer_id, final_skill_id, final_kill_damage);
#endif

	// Registrar kill (ou morte sem killer)
	if (killer_id > 0)
	{
		arena7x7_record_kill(match, killer_id, victim->status.char_id, final_skill_id, final_kill_damage);
	}
	else
	{
		// Apenas atualizar morte da v�tima (morte por ambiente, queda, etc)
		auto vit = match->players.find(victim->status.char_id);
		if (vit != match->players.end() && vit->second)
		{
			vit->second->deaths++;
			vit->second->current_streak = 0;
			vit->second->last_death_time = gettick();

			// Calcular tempo vivo at� este momento
			// Se j� tinha morrido antes, considera o tempo desde o join_time at� a �ltima morte
			// Como � morte permanente (tumba), s� morre uma vez
			if (vit->second->time_alive == 0)
			{
				vit->second->time_alive = (uint32)DIFF_TICK(gettick(), vit->second->join_time);
			}

			vit->second->recent_damage_taken.clear();
		}
	}
	// custom moskaum
	//  Se � um Shura com Cursed Circle ativo, remover de todos os alvos
	arena7x7_remove_cursedcircle_targets(victim);
	// fim do custom
	//  Transformar jogador em tumba (morte permanente)
	arena7x7_transform_to_tombstone(victim);

	// IMPORTANTE: Disparar evento de morte do BG manualmente
	// Como retornamos 0 em pc_dead(), o c�digo em status.cpp n�o chega a executar
	// o evento die_event, ent�o precisamos dispar�-lo aqui
	if (victim->bg_id)
	{
		std::shared_ptr<s_battleground_data> bg = util::umap_find(bg_team_db, victim->bg_id);
		if (bg && !(bg->die_event.empty()))
		{
			// #if ARENA7X7_DEBUG
			// 			ShowInfo("Arena7x7 DEBUG: Disparando die_event '%s' para %s\n",
			// 					 bg->die_event.c_str(), victim->status.name);
			// #endif
			npc_event(victim, bg->die_event.c_str(), 0);
		}
	}
}

/**
 * Hook chamado quando ocorre healing
 */
void arena7x7_on_heal(map_session_data *caster, map_session_data *target, uint16 skill_id, int heal_amount)
{
	if (!caster || !target || heal_amount <= 0)
		return;

	auto match = arena7x7_get_player_match(caster->status.char_id);
	if (!match)
		return;

	// Verificar se o alvo est� na mesma partida
	auto target_match = arena7x7_get_player_match(target->status.char_id);
	if (!target_match || target_match->match_id != match->match_id)
		return;

	// Verificar se s�o do mesmo time (heal em inimigo n�o conta como suporte)
	if (!arena7x7_same_team(caster->status.char_id, target->status.char_id))
		return;

	arena7x7_record_support(match, caster->status.char_id, target->status.char_id,
							skill_id, ARENA7X7_SUPPORT_HEAL, (uint32)heal_amount);
}

/**
 * Hook chamado quando um buff � aplicado
 */
void arena7x7_on_buff(map_session_data *caster, map_session_data *target, uint16 skill_id)
{
	if (!caster || !target)
		return;

	auto match = arena7x7_get_player_match(caster->status.char_id);
	if (!match)
		return;

	auto target_match = arena7x7_get_player_match(target->status.char_id);
	if (!target_match || target_match->match_id != match->match_id)
		return;

	// Verificar se s�o do mesmo time
	if (!arena7x7_same_team(caster->status.char_id, target->status.char_id))
	{
		// � debuff em inimigo
		arena7x7_record_support(match, caster->status.char_id, target->status.char_id,
								skill_id, ARENA7X7_SUPPORT_DEBUFF, 0);
	}
	else
	{
		// � buff em aliado
		arena7x7_record_support(match, caster->status.char_id, target->status.char_id,
								skill_id, ARENA7X7_SUPPORT_BUFF, 0);
	}
}

/**
 * Hook chamado quando um item � usado
 */
void arena7x7_on_item_use(map_session_data *sd, map_session_data *target,
						  t_itemid item_id, const char *item_name, int value)
{

	if (!sd)
		return;

	auto match = arena7x7_get_player_match(sd->status.char_id);
	if (!match)
		return;

	uint32 target_id = (target && target != sd) ? target->status.char_id : 0;

	// Determinar tipo de item
	e_arena7x7_item_type type = ARENA7X7_ITEM_OTHER;

	// IDs de po��es de HP comuns
	switch (item_id)
	{
	case 501:
	case 502:
	case 503:
	case 504:
	case 505:
	case 506: // Red Potions
	case 507:
	case 508: // White Potions
	case 545:
	case 546: // Condensed Potions
	case 11500:
	case 11501:
	case 11502: // Siege Potions
		type = ARENA7X7_ITEM_HP;
		break;
	case 509:
	case 510: // Blue Potions (SP)
		type = ARENA7X7_ITEM_SP;
		break;
	default:
		type = ARENA7X7_ITEM_OTHER;
		break;
	}

	arena7x7_record_item_use(match, sd->status.char_id, target_id, item_id, item_name, type, value);
}

/**
 * Rastreia itens consumidos durante a partida (chamado em pc_delitem)
 * Monitora uma lista espec�fica de itens importantes para PvP
 */
void arena7x7_track_item_consume(map_session_data *sd, t_itemid item_id, const char *item_name, int amount)
{
	if (!sd || amount <= 0)
		return;

	auto match = arena7x7_get_player_match(sd->status.char_id);
	if (!match || match->status != ARENA7X7_MATCH_ACTIVE)
		return;

	// Lista de itens que s�o requisitos de skills (n�o contam para APM)
	// Esses itens s�o consumidos automaticamente ao usar skills
	static const std::unordered_set<t_itemid> skill_requirement_items = {
		// Elemental Stones
		990,
		991,
		992,
		993,
		994,
		995,
		996,
		997, // Bloody_Red, Crystal_Blue, Wind_Of_Verdure, Yellow_Live, Flame_Heart, Mistic_Frozen, Rough_Wind, Great_Nature
		// Bullets
		6145,  // Vulcan_Bullet
		12383, // Vulcan_Bullet_Magazine
		// Cannon Balls
		18000,
		18001,
		18002,
		18003,
		18004, // Cannon_Ball, Holy_Cannon_Ball, etc
		// Mechanic items
		6146,
		6147, // Magic_Gear_Fuel, Liquid_Condensed_Bullet
		// Genetic items
		6210,
		6211,
		6212,
		6213,
		6214,
		6215,
		6216,
		6217, // Seeds, Bombs, Powders, etc
		7137,
		7138, // MenEater_Plant_Bottle, Mini_Bottle
		// Trap items
		1025,
		1065, // Spiderweb, Booby_Trap
		// Face/Surface Paint
		6120,
		6123, // Face_Paint, Surface_Paint
		// Gemstones
		716,
		717, // Red_Gemstone, Blue_Gemstone
		// Other catalysts
		757,  // Elunium_Stone
		7049, // Stone
	};

	// Lista de flechas - s� contam para APM quando usadas em ataque b�sico (related_skill_id == 0)
	// Quando usadas por skills (Arrow Storm, etc), N�O contam para APM
	static const std::unordered_set<t_itemid> arrow_items = {
		1750, // Arrow
		1751, // Silver_Arrow
		1752, // Fire_Arrow
		1753, // Steel_Arrow
		1754, // Crystal_Arrow
		1755, // Arrow_Of_Wind
		1756, // Stone_Arrow
		1757, // Immatrial_Arrow
		1762, // Rusty_Arrow
		1765, // Oridecon_Arrow
		1767, // Arrow_Of_Shadow
		1770, // Iron_Arrow
	};

	// Lista de IDs de itens consum�veis a serem monitorados (para APM)
	static const std::unordered_set<t_itemid> tracked_items = {
		// Po��es de HP b�sicas
		501,
		502,
		503,
		504,
		505,
		506, // Red, Orange, Yellow, White, Blue, Green Potion
		545,
		546,
		547, // Slim Potions (Red, Yellow, White)
		522, // Fruit of Mastela
		607, // Yggdrasil Berry
		608, // Yggdrasil Seed
		645, // Center Potion
		656, // Awakening Potion
		657, // Berserk Potion

		// HP Increase Potions
		12422,
		12423,
		12424, // HP_Increase_PotionS, M, L

		// WoE Potions
		11547,
		11548,
		11549, // Woe Violet, White, Blue Potion
		11500,
		11501,
		11502, // Siege Potions

		// Po��es de SP
		509,
		510,
		511,
		512,   // Blue Potions
		12427, // Ancilla (SP)
		12425,
		12426, // SP_Increase_PotionS, M, L

		// Holy Water e itens de cura de status
		523, // Holy Water
		525, // Panacea
		526, // Royal Jelly

		// Comidas de Stat (Stat Foods)
		12429, // Savage_BBQ (STR)
		12430, // Wug_Blood_Cocktail (INT)
		12431, // Minor_Brisket (VIT)
		12432, // Siroma_Icetea (DEX)
		12433, // Drocera_Herb_Stew (AGI)
		12434, // Petti_Tail_Noodle (LUK)
		12436, // Vitata500
		12437, // Enrich_Celermine_Juice

		// Elemental Potions
		12114,
		12115,
		12116,
		12117, // Elemental Fire, Water, Earth, Wind
		12118,
		12119,
		12120,
		12121, // More elemental

		// Resist Potions
		12118,
		12119,
		12120,
		12121,
		12122,
		12123,
		12124,
		12125,

		// Box/food items
		12004,
		12005,
		12006,
		12007,
		12008,
		12009,
		12010,
		12011,
		12012,
		12013,
		12014,
		12015,

		// Catalisadores importantes (itens de uso ativo)
		678,  // Poison Bottle
		7135, // Bottle Grenade
		7136, // Acid Bottle

		// Speed items
		601,
		602,
		605, // Fly Wing, Butterfly Wing, Anodyne
		12210,
		12211,
		12212, // Speed Potion, etc

		// Token/Special items
		12028,
		12029,
		12030,
		12031, // Token items
	};

	// Verificar se � um item que � requisito de skill (log mas n�o conta APM)
	bool is_skill_requirement = (skill_requirement_items.find(item_id) != skill_requirement_items.end());

	// Verificar se � uma flecha (tratamento especial)
	bool is_arrow = (arrow_items.find(item_id) != arrow_items.end());

	// Verificar se � um item rastre�vel (conta para APM)
	bool is_tracked = (tracked_items.find(item_id) != tracked_items.end());

	// Se n�o � nem requisito de skill, nem flecha, nem rastre�vel, ignorar
	if (!is_skill_requirement && !is_arrow && !is_tracked)
		return;

	// Determinar tipo de item
	e_arena7x7_item_type type = ARENA7X7_ITEM_OTHER;

	// Po��es de HP
	if ((item_id >= 501 && item_id <= 506) || item_id == 545 || item_id == 546 ||
		item_id == 547 || item_id == 522 || item_id == 607 || item_id == 645 ||
		item_id == 656 || item_id == 657 || (item_id >= 11547 && item_id <= 11549) ||
		(item_id >= 12392 && item_id <= 12394) || (item_id >= 12422 && item_id <= 12424))
	{
		type = ARENA7X7_ITEM_HP;
	}
	// Po��es de SP
	else if ((item_id >= 509 && item_id <= 512) || item_id == 12427 ||
			 item_id == 12425 || item_id == 12426)
	{
		type = ARENA7X7_ITEM_SP;
	}
	// Gemas (Yellow, Red, Blue Gemstone)
	else if (item_id == 714 || item_id == 715 || item_id == 716 || item_id == 717)
	{
		type = ARENA7X7_ITEM_GEMSTONE;
	}
	// Itens arremess�veis (Kunai, Shuriken, etc)
	else if ((item_id >= 1750 && item_id <= 1757) || item_id == 1762 ||
			 item_id == 1765 || item_id == 1767 || item_id == 1770 ||
			 (item_id >= 13265 && item_id <= 13268) || (item_id >= 13277 && item_id <= 13283))
	{
		type = ARENA7X7_ITEM_THROWING;
	}
	// Flechas e muni��es
	else if ((item_id >= 1750 && item_id <= 1770) || item_id == 1065 ||
			 (item_id >= 12004 && item_id <= 12015))
	{
		type = ARENA7X7_ITEM_ARROW;
	}
	// Catalisadores (Poison Bottle, etc)
	else if (item_id == 678 || item_id == 7940 || item_id == 6144 || item_id == 12383 ||
			 (item_id >= 6145 && item_id <= 6147) || item_id == 970 ||
			 (item_id >= 6210 && item_id <= 6217))
	{
		type = ARENA7X7_ITEM_CATALYST;
	}
	// Comidas/Stats (Stat Foods)
	else if ((item_id >= 12114 && item_id <= 12121) || (item_id >= 12717 && item_id <= 12724) ||
			 item_id == 12436 || item_id == 12437 || (item_id >= 12429 && item_id <= 12434))
	{
		type = ARENA7X7_ITEM_FOOD;
	}
	// Buffs diversos (Holy Water = 523)
	else if (item_id == 523 || item_id == 525 || item_id == 526 || item_id == 11513 ||
			 (item_id >= 18000 && item_id <= 18004) || (item_id >= 7137 && item_id <= 7138) ||
			 item_id == 6120 || item_id == 6123 || item_id == 6128)
	{
		type = ARENA7X7_ITEM_BUFF;
	}

	// Converter tipo para string
	std::string type_str;
	switch (type)
	{
	case ARENA7X7_ITEM_HP:
		type_str = "hp_potion";
		break;
	case ARENA7X7_ITEM_SP:
		type_str = "sp_potion";
		break;
	case ARENA7X7_ITEM_GEMSTONE:
		type_str = "gemstone";
		break;
	case ARENA7X7_ITEM_THROWING:
		type_str = "throwing";
		break;
	case ARENA7X7_ITEM_ARROW:
		type_str = "arrow";
		break;
	case ARENA7X7_ITEM_CATALYST:
		type_str = "catalyst";
		break;
	case ARENA7X7_ITEM_FOOD:
		type_str = "food";
		break;
	case ARENA7X7_ITEM_BUFF:
		type_str = "buff";
		break;
	default:
		type_str = "other";
		break;
	}

	// Criar entrada de log para item_usage_log (usado pelo salvamento)
	s_arena7x7_item_log log_entry;
	log_entry.char_id = sd->status.char_id;
	log_entry.char_name = sd->status.name;
	log_entry.item_id = item_id;
	log_entry.item_name = item_name ? item_name : "";
	log_entry.amount = amount;
	log_entry.item_type = type_str;
	log_entry.related_skill_id = 0;
	log_entry.timestamp = gettick();

	match->item_usage_log.push_back(log_entry);

	// Tamb�m registrar no item_log antigo para estat�sticas
	for (int i = 0; i < amount; i++)
	{
		arena7x7_record_item_use(match, sd->status.char_id, 0, item_id, item_name, type, 1);
	}

	// Atualizar contador de itens usados (apenas para itens rastreados, n�o requisitos de skill)
	// Itens que s�o requisitos de skill (gemas, flechas, etc) n�o contam para APM
	if (is_tracked && !is_skill_requirement)
	{
		auto it = match->players.find(sd->status.char_id);
		if (it != match->players.end() && it->second)
		{
			it->second->total_items_used += amount;
		}
	}
}

/**
 * Hook chamado quando SP � consumido
 */
void arena7x7_on_sp_consume(map_session_data *sd, int sp_amount)
{
	if (!sd || sp_amount <= 0)
		return;

	auto match = arena7x7_get_player_match(sd->status.char_id);
	if (!match)
		return;

	arena7x7_record_sp_use(match, sd->status.char_id, (uint32)sp_amount);
}

// ============================================================================
// Fun��es de Log Detalhado para o Site
// ============================================================================

/**
 * Hook para logar uso detalhado de skill com dano
 * Chamado em skill.cpp quando uma skill causa dano
 */
void arena7x7_log_skill_damage(map_session_data *caster, struct block_list *target,
							   uint16 skill_id, uint16 skill_lv, int64 damage, int hit_count, bool is_critical)
{

	if (!caster || !target || damage <= 0)
		return;
	if (target->type != BL_PC)
		return; // S� logar dano em jogadores

	auto match = arena7x7_get_player_match(caster->status.char_id);
	if (!match)
		return;

	map_session_data *target_sd = (map_session_data *)target;

	// Verificar se o alvo est� na mesma partida
	auto target_match = arena7x7_get_player_match(target_sd->status.char_id);
	if (!target_match || target_match->match_id != match->match_id)
		return;

	// Verificar se s�o times diferentes (n�o logar dano em aliados)
	if (arena7x7_same_team(caster->status.char_id, target_sd->status.char_id))
		return;

	// Criar entrada de log de skill
	s_arena7x7_skill_log log_entry;
	log_entry.caster_id = caster->status.char_id;
	log_entry.caster_name = caster->status.name;
	log_entry.target_id = target_sd->status.char_id;
	log_entry.target_name = target_sd->status.name;
	log_entry.skill_id = skill_id;
	log_entry.skill_name = skill_id > 0 ? skill_get_name(skill_id) : "Ataque Normal";
	log_entry.skill_level = skill_lv;
	log_entry.damage = (uint32)damage;
	log_entry.hits = (uint16)hit_count;
	log_entry.is_critical = is_critical;
	log_entry.is_kill = (target_sd->battle_status.hp <= 0);
	log_entry.timestamp = gettick();

	match->skill_log.push_back(log_entry);
}

/**
 * Hook para contar uso de skill (todas as skills, incluindo suporte)
 * Incrementa total_skills_used nas estat�sticas do jogador
 * e registra cada skill individualmente no mapa skills_used_map
 */
void arena7x7_count_skill_use(map_session_data *sd, uint16 skill_id)
{
	if (!sd || skill_id == 0)
		return;

	auto stats = arena7x7_get_player_stats(sd->status.char_id);
	if (!stats)
		return;

	// Incrementar contador total de skills usadas
	stats->total_skills_used++;

	// Registrar skill espec�fica no mapa
	auto &skill_entry = stats->skills_used_map[skill_id];
	skill_entry.first++; // Incrementar contador de uso
	if (skill_entry.second.empty())
	{
		// Obter nome da skill se ainda n�o temos
		const char *skill_name = skill_get_name(skill_id);
		skill_entry.second = skill_name ? skill_name : "Unknown Skill";
	}
}

/**
 * Hook para logar consumo de itens por skills (flechas, gemas, catalisadores)
 */
void arena7x7_log_skill_item_consumption(map_session_data *sd, t_itemid item_id,
										 uint16 amount, uint16 skill_id, const char *item_type)
{

	if (!sd || amount == 0)
		return;

	auto match = arena7x7_get_player_match(sd->status.char_id);
	if (!match)
		return;

	// Obter nome do item do itemdb
	std::shared_ptr<item_data> id = item_db.find(item_id);
	std::string item_name = id ? id->ename : "Unknown Item";

	// Atualizar contador de itens usados nas estat�sticas
	auto stats = arena7x7_get_player_stats(sd->status.char_id);
	if (stats)
	{
		stats->total_items_used += amount;
	}

	// Criar entrada de log
	s_arena7x7_item_log log_entry;
	log_entry.char_id = sd->status.char_id;
	log_entry.char_name = sd->status.name;
	log_entry.item_id = item_id;
	log_entry.item_name = item_name;
	log_entry.amount = amount;
	log_entry.item_type = item_type ? item_type : "other";
	log_entry.related_skill_id = skill_id;
	log_entry.timestamp = gettick();

	match->item_usage_log.push_back(log_entry);
}

/**
 * Hook para logar uso de flechas em ataques
 * @param skill_id - ID da skill que consumiu a flecha (0 = ataque b�sico)
 * Flechas s� contam para APM quando usadas em ataque b�sico (skill_id == 0)
 */
void arena7x7_log_arrow_consumption(map_session_data *sd, t_itemid arrow_id, uint16 amount, int32 skill_id)
{
	if (!sd || amount == 0)
		return;

	auto match = arena7x7_get_player_match(sd->status.char_id);
	if (!match)
		return;

	// Obter nome do item
	std::shared_ptr<item_data> id = item_db.find(arrow_id);
	std::string item_name = id ? id->ename : "Unknown Arrow";

	// Atualizar contador de itens usados nas estat�sticas
	// APENAS se for ataque b�sico (skill_id == 0)
	// Flechas consumidas por skills (Arrow Storm, etc) N�O contam para APM
	if (skill_id == 0)
	{
		auto stats = arena7x7_get_player_stats(sd->status.char_id);
		if (stats)
		{
			stats->total_items_used += amount;
		}
	}

	// Criar entrada de log (sempre loga, independente de ser skill ou n�o)
	s_arena7x7_item_log log_entry;
	log_entry.char_id = sd->status.char_id;
	log_entry.char_name = sd->status.name;
	log_entry.item_id = arrow_id;
	log_entry.item_name = item_name;
	log_entry.amount = amount;
	log_entry.item_type = "arrow";
	log_entry.related_skill_id = skill_id; // 0 = ataque normal, >0 = skill
	log_entry.timestamp = gettick();

	match->item_usage_log.push_back(log_entry);
}

/**
 * Hook para logar ataque normal detalhado
 */
void arena7x7_log_normal_attack(map_session_data *attacker, struct block_list *target,
								int64 damage, bool is_critical, bool is_miss)
{

	if (!attacker || !target)
		return;
	if (target->type != BL_PC)
		return;

	auto match = arena7x7_get_player_match(attacker->status.char_id);
	if (!match)
		return;

	map_session_data *target_sd = (map_session_data *)target;

	// Verificar se o alvo est� na mesma partida
	auto target_match = arena7x7_get_player_match(target_sd->status.char_id);
	if (!target_match || target_match->match_id != match->match_id)
		return;

	// Verificar se s�o times diferentes
	if (arena7x7_same_team(attacker->status.char_id, target_sd->status.char_id))
		return;

	// Criar chave �nica attacker->target
	uint64 key = ((uint64)attacker->status.char_id << 32) | target_sd->status.char_id;

	// Atualizar ou criar entrada
	auto &entry = match->attack_log[key];
	if (entry.attacker_id == 0)
	{
		// Nova entrada
		entry.attacker_id = attacker->status.char_id;
		entry.attacker_name = attacker->status.name;
		entry.target_id = target_sd->status.char_id;
		entry.target_name = target_sd->status.name;
		entry.total_damage = 0;
		entry.hit_count = 0;
		entry.critical_count = 0;
		entry.miss_count = 0;
	}

	if (is_miss)
	{
		entry.miss_count++;
	}
	else
	{
		entry.hit_count++;
		entry.total_damage += damage;
		if (is_critical)
		{
			entry.critical_count++;
		}
	}
}

/**
 * Salvar logs detalhados no banco de dados
 * Otimizado com INSERT em lote e transacao SQL para evitar travamentos
 */
void arena7x7_save_detailed_logs(uint32 match_id)
{
	auto it = arena7x7_matches.find(match_id);
	if (it == arena7x7_matches.end() || !it->second)
		return;

	auto match = it->second;
	const size_t BATCH_SIZE = 100;

	// === INICIAR TRANSACAO PARA PERFORMANCE ===
	Sql_Query(mmysql_handle, "START TRANSACTION");

	// Salvar logs de skills - INSERT em lote
	if (!match->skill_log.empty())
	{
		std::string skill_batch;
		size_t batch_count = 0;
		bool first = true;

		for (const auto &log : match->skill_log)
		{
			char esc_caster_name[NAME_LENGTH * 2 + 1];
			char esc_target_name[NAME_LENGTH * 2 + 1];
			char esc_skill_name[SKILL_NAME_LENGTH * 2 + 1];
			Sql_EscapeStringLen(mmysql_handle, esc_caster_name, log.caster_name.c_str(), log.caster_name.length());
			Sql_EscapeStringLen(mmysql_handle, esc_target_name, log.target_name.c_str(), log.target_name.length());
			Sql_EscapeStringLen(mmysql_handle, esc_skill_name, log.skill_name.c_str(), log.skill_name.length());

			char value_str[1024];
			snprintf(value_str, sizeof(value_str), "%s(%u, %u, %u, '%s', %u, '%s', %u, '%s', %u, %u, %u, %d, %d)",
					 first ? "" : ",",
					 match_id, (uint32)log.timestamp, log.caster_id, esc_caster_name,
					 log.target_id, esc_target_name, log.skill_id, esc_skill_name,
					 log.skill_level, log.damage, log.hits, log.is_critical ? 1 : 0, log.is_kill ? 1 : 0);

			if (first)
			{
				skill_batch = "INSERT INTO `arena7x7_skill_log` "
							  "(`match_id`, `timestamp`, `caster_id`, `caster_name`, `target_id`, `target_name`, "
							  "`skill_id`, `skill_name`, `skill_level`, `damage`, `hits`, `is_critical`, `is_kill`) VALUES ";
			}
			skill_batch += value_str;
			first = false;
			batch_count++;

			if (batch_count >= BATCH_SIZE)
			{
				Sql_QueryStr(mmysql_handle, skill_batch.c_str());
				skill_batch.clear();
				first = true;
				batch_count = 0;
			}
		}
		if (!first)
			Sql_QueryStr(mmysql_handle, skill_batch.c_str());
	}

	// Salvar logs de itens consumidos - INSERT em lote
	if (!match->item_usage_log.empty())
	{
		std::string item_batch;
		size_t batch_count = 0;
		bool first = true;

		for (const auto &log : match->item_usage_log)
		{
			char esc_char_name[NAME_LENGTH * 2 + 1];
			char esc_item_name[100 * 2 + 1];
			char esc_item_type[50 * 2 + 1];
			Sql_EscapeStringLen(mmysql_handle, esc_char_name, log.char_name.c_str(), log.char_name.length());
			Sql_EscapeStringLen(mmysql_handle, esc_item_name, log.item_name.c_str(), log.item_name.length());
			Sql_EscapeStringLen(mmysql_handle, esc_item_type, log.item_type.c_str(), log.item_type.length());

			char value_str[512];
			snprintf(value_str, sizeof(value_str), "%s(%u, %u, %u, '%s', %u, '%s', %u, '%s', %u)",
					 first ? "" : ",",
					 match_id, (uint32)log.timestamp, log.char_id, esc_char_name,
					 log.item_id, esc_item_name, log.amount, esc_item_type, log.related_skill_id);

			if (first)
			{
				item_batch = "INSERT INTO `arena7x7_item_log` "
							 "(`match_id`, `timestamp`, `char_id`, `char_name`, `item_id`, `item_name`, "
							 "`amount`, `item_type`, `related_skill_id`) VALUES ";
			}
			item_batch += value_str;
			first = false;
			batch_count++;

			if (batch_count >= BATCH_SIZE)
			{
				Sql_QueryStr(mmysql_handle, item_batch.c_str());
				item_batch.clear();
				first = true;
				batch_count = 0;
			}
		}
		if (!first)
			Sql_QueryStr(mmysql_handle, item_batch.c_str());
	}

	// Salvar logs de ataques normais (agregados) - INSERT em lote
	if (!match->attack_log.empty())
	{
		std::string attack_batch = "INSERT INTO `arena7x7_attack_log` "
								   "(`match_id`, `attacker_id`, `attacker_name`, `target_id`, `target_name`, "
								   "`total_damage`, `hit_count`, `critical_count`, `miss_count`) VALUES ";
		bool first = true;

		for (const auto &kv : match->attack_log)
		{
			const auto &log = kv.second;
			char esc_attacker_name[NAME_LENGTH * 2 + 1];
			char esc_target_name[NAME_LENGTH * 2 + 1];
			Sql_EscapeStringLen(mmysql_handle, esc_attacker_name, log.attacker_name.c_str(), log.attacker_name.length());
			Sql_EscapeStringLen(mmysql_handle, esc_target_name, log.target_name.c_str(), log.target_name.length());

			char value_str[512];
			snprintf(value_str, sizeof(value_str), "%s(%u, %u, '%s', %u, '%s', %lld, %u, %u, %u)",
					 first ? "" : ",",
					 match_id, log.attacker_id, esc_attacker_name,
					 log.target_id, esc_target_name,
					 (long long)log.total_damage, log.hit_count, log.critical_count, log.miss_count);
			attack_batch += value_str;
			first = false;
		}
		Sql_QueryStr(mmysql_handle, attack_batch.c_str());
	}

	// Criar resumos agregados de skills por jogador e alvo
	std::unordered_map<uint64, std::tuple<uint32, std::string, uint16, std::string, uint32, uint32, uint32, uint32>> skill_summary;
	// key = (char_id << 32) | skill_id -> (char_id, char_name, skill_id, skill_name, total_damage, hits, uses, kills)

	for (const auto &log : match->skill_log)
	{
		uint64 key = ((uint64)log.caster_id << 32) | log.skill_id;
		auto &s = skill_summary[key];
		if (std::get<0>(s) == 0)
		{
			std::get<0>(s) = log.caster_id;
			std::get<1>(s) = log.caster_name;
			std::get<2>(s) = log.skill_id;
			std::get<3>(s) = log.skill_name;
		}
		std::get<4>(s) += log.damage;
		std::get<5>(s) += log.hits;
		std::get<6>(s)++; // uses
		if (log.is_kill)
			std::get<7>(s)++;
	}

	// Salvar resumo de skills - INSERT em lote com ON DUPLICATE KEY
	if (!skill_summary.empty())
	{
		for (const auto &kv : skill_summary)
		{
			const auto &s = kv.second;
			char esc_char_name[NAME_LENGTH * 2 + 1];
			char esc_skill_name[SKILL_NAME_LENGTH * 2 + 1];
			Sql_EscapeStringLen(mmysql_handle, esc_char_name, std::get<1>(s).c_str(), std::get<1>(s).length());
			Sql_EscapeStringLen(mmysql_handle, esc_skill_name, std::get<3>(s).c_str(), std::get<3>(s).length());

			Sql_Query(mmysql_handle,
					  "INSERT INTO `arena7x7_skill_summary` "
					  "(`match_id`, `char_id`, `char_name`, `skill_id`, `skill_name`, "
					  "`total_damage`, `total_hits`, `use_count`, `kills_with_skill`) "
					  "VALUES (%u, %u, '%s', %u, '%s', %u, %u, %u, %u) "
					  "ON DUPLICATE KEY UPDATE "
					  "`total_damage` = `total_damage` + VALUES(`total_damage`), "
					  "`total_hits` = `total_hits` + VALUES(`total_hits`), "
					  "`use_count` = `use_count` + VALUES(`use_count`), "
					  "`kills_with_skill` = `kills_with_skill` + VALUES(`kills_with_skill`)",
					  match_id, std::get<0>(s), esc_char_name, std::get<2>(s), esc_skill_name,
					  std::get<4>(s), std::get<5>(s), std::get<6>(s), std::get<7>(s));
		}
	}

	// Criar resumo de dano por alvo (skill + normal)
	std::unordered_map<uint64, std::tuple<uint32, std::string, uint32, std::string, uint32, uint32>> damage_by_target_summary;
	// key = (attacker_id << 32) | target_id -> (attacker_id, name, target_id, name, skill_damage, normal_damage)

	// Somar dano de skills
	for (const auto &log : match->skill_log)
	{
		uint64 key = ((uint64)log.caster_id << 32) | log.target_id;
		auto &d = damage_by_target_summary[key];
		if (std::get<0>(d) == 0)
		{
			std::get<0>(d) = log.caster_id;
			std::get<1>(d) = log.caster_name;
			std::get<2>(d) = log.target_id;
			std::get<3>(d) = log.target_name;
		}
		std::get<4>(d) += log.damage;
	}

	// Somar dano de ataques normais
	for (const auto &kv : match->attack_log)
	{
		uint64 key = kv.first;
		const auto &log = kv.second;
		auto &d = damage_by_target_summary[key];
		if (std::get<0>(d) == 0)
		{
			std::get<0>(d) = log.attacker_id;
			std::get<1>(d) = log.attacker_name;
			std::get<2>(d) = log.target_id;
			std::get<3>(d) = log.target_name;
		}
		std::get<5>(d) += (uint32)log.total_damage;
	}

	// Salvar resumo de dano por alvo - com ON DUPLICATE KEY
	for (const auto &kv : damage_by_target_summary)
	{
		const auto &d = kv.second;
		uint32 total_damage = std::get<4>(d) + std::get<5>(d);
		if (total_damage == 0)
			continue;

		char esc_attacker_name[NAME_LENGTH * 2 + 1];
		char esc_target_name[NAME_LENGTH * 2 + 1];
		Sql_EscapeStringLen(mmysql_handle, esc_attacker_name, std::get<1>(d).c_str(), std::get<1>(d).length());
		Sql_EscapeStringLen(mmysql_handle, esc_target_name, std::get<3>(d).c_str(), std::get<3>(d).length());

		Sql_Query(mmysql_handle,
				  "INSERT INTO `arena7x7_damage_by_target` "
				  "(`match_id`, `attacker_id`, `attacker_name`, `target_id`, `target_name`, "
				  "`skill_damage`, `normal_damage`, `total_damage`) "
				  "VALUES (%u, %u, '%s', %u, '%s', %u, %u, %u) "
				  "ON DUPLICATE KEY UPDATE "
				  "`skill_damage` = VALUES(`skill_damage`), "
				  "`normal_damage` = VALUES(`normal_damage`), "
				  "`total_damage` = VALUES(`total_damage`)",
				  match_id, std::get<0>(d), esc_attacker_name, std::get<2>(d), esc_target_name,
				  std::get<4>(d), std::get<5>(d), total_damage);
	}

	// Criar resumo de itens consumidos
	std::unordered_map<uint64, std::tuple<uint32, std::string, t_itemid, std::string, std::string, uint32>> item_summary;
	// key = (char_id << 32) | item_id -> (char_id, char_name, item_id, item_name, item_type, total_amount)

	for (const auto &log : match->item_usage_log)
	{
		uint64 key = ((uint64)log.char_id << 32) | log.item_id;
		auto &i = item_summary[key];
		if (std::get<0>(i) == 0)
		{
			std::get<0>(i) = log.char_id;
			std::get<1>(i) = log.char_name;
			std::get<2>(i) = log.item_id;
			std::get<3>(i) = log.item_name;
			std::get<4>(i) = log.item_type;
		}
		std::get<5>(i) += log.amount;
	}

	// Salvar resumo de itens - INSERT em lote
	if (!item_summary.empty())
	{
		std::string item_summary_batch = "INSERT INTO `arena7x7_item_summary` "
										 "(`match_id`, `char_id`, `char_name`, `item_id`, `item_name`, `item_type`, `total_amount`) VALUES ";
		bool first = true;

		for (const auto &kv : item_summary)
		{
			const auto &i = kv.second;
			char esc_char_name[NAME_LENGTH * 2 + 1];
			char esc_item_name[100 * 2 + 1];
			char esc_item_type[50 * 2 + 1];
			Sql_EscapeStringLen(mmysql_handle, esc_char_name, std::get<1>(i).c_str(), std::get<1>(i).length());
			Sql_EscapeStringLen(mmysql_handle, esc_item_name, std::get<3>(i).c_str(), std::get<3>(i).length());
			Sql_EscapeStringLen(mmysql_handle, esc_item_type, std::get<4>(i).c_str(), std::get<4>(i).length());

			char value_str[512];
			snprintf(value_str, sizeof(value_str), "%s(%u, %u, '%s', %u, '%s', '%s', %u)",
					 first ? "" : ",",
					 match_id, std::get<0>(i), esc_char_name, (uint32)std::get<2>(i),
					 esc_item_name, esc_item_type, std::get<5>(i));
			item_summary_batch += value_str;
			first = false;
		}
		Sql_QueryStr(mmysql_handle, item_summary_batch.c_str());
	}

	// === FINALIZAR TRANSACAO ===
	Sql_Query(mmysql_handle, "COMMIT");

	// #if ARENA7X7_DEBUG
	// 	ShowInfo("Arena7x7: Logs detalhados salvos para match %u (skills: %zu, items: %zu, attacks: %zu)\n",
	// 			 match_id, match->skill_log.size(), match->item_usage_log.size(), match->attack_log.size());
	// #endif
}

// ============================================================================
// Inicializa��o e Finaliza��o
// ============================================================================

void do_init_arena7x7(void)
{
	// Carregar pr�ximo match_id do banco
	arena7x7_match_counter = arena7x7_load_next_match_id() - 1;

	// Carregar temporada atual
	arena7x7_current_season = arena7x7_get_current_season();

	ShowStatus("Arena7x7: Sistema inicializado (proximo match_id: %u, season: %u)\n",
			   arena7x7_match_counter + 1, arena7x7_current_season);
}

void do_final_arena7x7(void)
{
	// Finalizar partidas ativas (marcando como canceladas)
	for (auto &kv : arena7x7_matches)
	{
		if (kv.second && kv.second->status == ARENA7X7_MATCH_ACTIVE)
		{
			// showwarning("Arena7x7: Cancelando partida %u por shutdown\n", kv.first);
			kv.second->status = ARENA7X7_MATCH_CANCELLED;
			arena7x7_save_match(kv.second);
		}
	}

	arena7x7_matches.clear();
	arena7x7_player_match.clear();

	ShowStatus("Arena7x7: Sistema finalizado\n");
}
