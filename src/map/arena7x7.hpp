// Copyright (c) rAthena Dev Teams - Licensed under GNU GPL
// For more information, see LICENCE in the main folder

/**
 * =============================================================================
 * Arena 7x7 - Sistema de Batalha PvP em Equipes
 * =============================================================================
 *
 * Este arquivo cont�m as defini��es de estruturas e fun��es para o sistema
 * de arena 7x7, onde duas equipes se enfrentam em batalhas PvP.
 *
 * Principais caracter�sticas:
 * - Equipes de at� 7 jogadores cada
 * - Sistema de morte permanente (jogador vira tumba at� o fim da partida)
 * - Tracking completo de estat�sticas (dano, kills, assists, healing, itens)
 * - Integra��o com sistema de ranking e temporadas
 * - Persist�ncia em banco de dados para an�lise no site
 *
 * Arquivos relacionados:
 * - arena7x7.cpp: Implementa��o das fun��es
 * - npc/custom/arena7x7_*.txt: Scripts NPC do sistema
 * - sql-files/arena7x7_*.sql: Esquemas do banco de dados
 *
 * =============================================================================
 */

#ifndef ARENA7X7_HPP
#define ARENA7X7_HPP

#include <memory>
#include <unordered_map>
#include <vector>
#include <string>

#include "../common/cbasetypes.hpp"
#include "../common/database.hpp"
#include "../common/mmo.hpp"
#include "../common/timer.hpp"

// Forward declarations
class map_session_data;
struct block_list;

// ============================================================================
// Constantes de Configura��o
// ============================================================================

/// Habilitar mensagens de debug (0 = desabilitado, 1 = habilitado)
#define ARENA7X7_DEBUG 0

/// N�mero m�ximo de jogadores por time (7 jogadores = arena 7x7)
#define ARENA7X7_MAX_PLAYERS_PER_TEAM 7

/// Total de jogadores em uma partida (2 times * 7 jogadores)
#define ARENA7X7_TOTAL_PLAYERS 14

/// Janela de tempo para contabilizar assist�ncias (em milissegundos)
/// Se um jogador causou dano em algu�m nos �ltimos 5 segundos antes da morte,
/// ele recebe uma assist�ncia
#define ARENA7X7_ASSIST_WINDOW 5000

// ============================================================================
// Enumera��es
// ============================================================================

/**
 * Identificadores dos times na arena
 * NONE: Jogador n�o est� em nenhum time
 * BLUE: Time azul (usa bgscore_lion no placar)
 * RED: Time vermelho (usa bgscore_eagle no placar)
 */
enum e_arena7x7_team : uint8
{
	ARENA7X7_TEAM_NONE = 0,
	ARENA7X7_TEAM_BLUE = 1,
	ARENA7X7_TEAM_RED = 2
};

/**
 * Status de uma partida
 * WAITING: Aguardando jogadores entrarem
 * ACTIVE: Partida em andamento
 * FINISHED: Partida finalizada normalmente
 * CANCELLED: Partida cancelada (por desconex�o, shutdown, etc)
 */
enum e_arena7x7_match_status : uint8
{
	ARENA7X7_MATCH_WAITING = 0,
	ARENA7X7_MATCH_ACTIVE = 1,
	ARENA7X7_MATCH_FINISHED = 2,
	ARENA7X7_MATCH_CANCELLED = 3
};

/**
 * Tipos de skill de suporte (para categoriza��o nas estat�sticas)
 */
enum e_arena7x7_support_type : uint8
{
	ARENA7X7_SUPPORT_HEAL = 1,	 ///< Cura (Heal, Sanctuary, etc)
	ARENA7X7_SUPPORT_BUFF = 2,	 ///< Buff em aliado (Blessing, AGI Up, etc)
	ARENA7X7_SUPPORT_DEBUFF = 3, ///< Debuff em inimigo (Decrease AGI, etc)
	ARENA7X7_SUPPORT_DISPEL = 4	 ///< Dispel de buffs
};

/**
 * Tipos de itens consum�veis (para categoriza��o nas estat�sticas)
 */
enum e_arena7x7_item_type : uint8
{
	ARENA7X7_ITEM_HP = 1,		///< Po��es de HP (Red Potion, White Potion, etc)
	ARENA7X7_ITEM_SP = 2,		///< Po��es de SP (Blue Potion, etc)
	ARENA7X7_ITEM_BUFF = 3,		///< Itens de buff (Box of Sunlight, etc)
	ARENA7X7_ITEM_OTHER = 4,	///< Outros itens
	ARENA7X7_ITEM_GEMSTONE = 5, ///< Gemas (Yellow, Red, Blue Gemstone)
	ARENA7X7_ITEM_ARROW = 6,	///< Flechas e muni��es
	ARENA7X7_ITEM_CATALYST = 7, ///< Catalisadores (Poison Bottle, etc)
	ARENA7X7_ITEM_FOOD = 8,		///< Comidas/Stats
	ARENA7X7_ITEM_THROWING = 9	///< Itens arremess�veis
};

// ============================================================================
// Estruturas de Dados
// ============================================================================

/**
 * Estrutura para tracking de dano recente (usado para calcular assist�ncias)
 * Quando um jogador causa dano em outro, registramos o atacante e timestamp.
 * Se a v�tima morrer dentro de ARENA7X7_ASSIST_WINDOW, os atacantes recentes
 * (exceto o killer) recebem assist�ncia.
 */
struct s_arena7x7_recent_damage
{
	uint32 attacker_id; ///< ID do jogador que causou dano
	uint16 skill_id;    ///< ID da skill que causou o dano (0 = ataque normal)
	uint32 damage;      ///< Quantidade de dano causado
	t_tick timestamp;	///< Momento em que o dano foi causado
};

/**
 * Estat�sticas de um jogador em uma partida espec�fica
 * Cont�m todos os dados de performance que ser�o salvos no banco de dados
 * e exibidos no site de estat�sticas.
 */
struct s_arena7x7_player_stats
{
	// Identifica��o do jogador
	uint32 char_id;			///< ID do personagem
	uint32 account_id;		///< ID da conta
	std::string char_name;	///< Nome do personagem
	e_arena7x7_team team;	///< Time do jogador (azul/vermelho)
	uint32 guild_id;		///< ID da guild (se tiver)
	std::string guild_name; ///< Nome da guild
	uint16 job_class;		///< Classe do personagem
	uint16 base_level;		///< N�vel base
	bool is_leader;			///< Se � l�der do time
	bool is_deserter;		///< Se abandonou a partida
	bool is_dead;			///< Se est� morto (tumba) - n�o pode ressuscitar

	// Estat�sticas de dano
	uint32 damage_done;		///< Total de dano causado
	uint32 damage_received; ///< Total de dano recebido
	uint32 top_damage;		///< Maior dano em um �nico hit

	// Estat�sticas de combate
	uint16 kills;		   ///< N�mero de kills
	uint16 deaths;		   ///< N�mero de mortes
	uint16 assists;		   ///< N�mero de assist�ncias
	uint16 current_streak; ///< Sequ�ncia atual de kills
	uint16 best_streak;	   ///< Melhor sequ�ncia de kills na partida

	// Estat�sticas de suporte
	uint32 healing_done;		///< HP curado em aliados
	uint32 healing_received;	///< HP recebido de cura
	uint16 support_skills_used; ///< Quantidade de skills de suporte usadas
	uint16 buffs_given;			///< Buffs dados em aliados
	uint16 debuffs_given;		///< Debuffs aplicados em inimigos
	uint32 total_skills_used;	///< Total de todas as skills usadas
	uint32 total_items_used;	///< Total de todos os itens consumidos

	// Consum�veis usados
	uint16 hp_potions;	   ///< Po��es de HP usadas
	uint16 sp_potions;	   ///< Po��es de SP usadas
	uint16 yellow_gems;	   ///< Yellow Gemstones consumidas
	uint16 red_gems;	   ///< Red Gemstones consumidas
	uint16 blue_gems;	   ///< Blue Gemstones consumidas
	uint16 poison_bottles; ///< Poison Bottles usadas
	uint16 other_items;	   ///< Outros itens usados

	// Recursos consumidos
	uint32 sp_consumed;	  ///< SP total gasto em skills
	uint32 zeny_consumed; ///< Zeny gasto (se aplic�vel)

	// Tracking de tempo
	t_tick join_time;		///< Momento que entrou na partida
	t_tick last_death_time; ///< Momento da �ltima morte
	uint32 time_alive;		///< Tempo vivo (em ms)
	uint32 time_dead;		///< Tempo morto (em ms)

	// Lista de danos recentes para c�lculo de assist�ncias
	std::vector<s_arena7x7_recent_damage> recent_damage_taken;

	// Mapa de skills usadas: skill_id -> {use_count, skill_name}
	std::unordered_map<uint16, std::pair<uint32, std::string>> skills_used_map;

	s_arena7x7_player_stats()
		: char_id(0), account_id(0), char_name(), team(ARENA7X7_TEAM_NONE),
		  guild_id(0), guild_name(), job_class(0), base_level(0),
		  is_leader(false), is_deserter(false), is_dead(false),
		  damage_done(0), damage_received(0), top_damage(0),
		  kills(0), deaths(0), assists(0), current_streak(0), best_streak(0),
		  healing_done(0), healing_received(0), support_skills_used(0),
		  buffs_given(0), debuffs_given(0), total_skills_used(0), total_items_used(0),
		  hp_potions(0), sp_potions(0), yellow_gems(0), red_gems(0), blue_gems(0),
		  poison_bottles(0), other_items(0), sp_consumed(0), zeny_consumed(0),
		  join_time(0), last_death_time(0), time_alive(0), time_dead(0)
	{
		// C++ objects (std::string, std::vector, std::unordered_map) are automatically initialized
	}
};

/// Registro de dano detalhado
struct s_arena7x7_damage_entry
{
	uint32 attacker_id;
	uint32 target_id;
	uint16 skill_id;
	uint32 damage;
	bool is_critical;
	bool is_kill;
	t_tick timestamp;
};

/// Registro de suporte detalhado
struct s_arena7x7_support_entry
{
	uint32 caster_id;
	uint32 target_id;
	uint16 skill_id;
	e_arena7x7_support_type type;
	uint32 value;
	t_tick timestamp;
};

/// Registro de item usado
struct s_arena7x7_item_entry
{
	uint32 char_id;
	uint32 target_id; // 0 se usado em si mesmo
	t_itemid item_id;
	std::string item_name;
	e_arena7x7_item_type type;
	uint32 value;
	t_tick timestamp;
};

/// Registro de kill
struct s_arena7x7_kill_entry
{
	uint32 killer_id;
	uint32 victim_id;
	uint16 skill_id;
	uint32 kill_damage;  ///< Dano final que causou a morte
	uint32 assist1_id;
	uint32 assist2_id;
	uint32 assist3_id;
	uint16 killer_streak;
	t_tick timestamp;
};

/// Dados agregados de dano por skill
struct s_arena7x7_damage_by_skill
{
	uint32 char_id;
	uint16 skill_id;
	std::string skill_name;
	uint32 total_damage;
	uint16 hit_count;
	uint16 critical_count;
	uint16 kill_count;
	uint32 max_damage;
};

/// Dados agregados de dano por alvo
struct s_arena7x7_damage_by_target
{
	uint32 attacker_id;
	uint32 target_id;
	uint32 total_damage;
	uint16 hit_count;
	uint16 kill_count;
};

/// Dados agregados de suporte por alvo
struct s_arena7x7_support_by_target
{
	uint32 caster_id;
	uint32 target_id;
	uint32 total_healing;
	uint16 buff_count;
	uint16 skill_count;
};

/// Log detalhado de skill usada (para salvar no banco)
struct s_arena7x7_skill_log
{
	uint32 caster_id;
	std::string caster_name;
	uint32 target_id;
	std::string target_name;
	uint16 skill_id;
	std::string skill_name;
	uint16 skill_level;
	uint32 damage;
	uint16 hits;
	bool is_critical;
	bool is_kill;
	t_tick timestamp;
};

/// Log detalhado de item consumido
struct s_arena7x7_item_log
{
	uint32 char_id;
	std::string char_name;
	t_itemid item_id;
	std::string item_name;
	uint16 amount;
	std::string item_type; // arrow, gemstone, potion, catalyst, other
	uint16 related_skill_id;
	t_tick timestamp;
};

/// Log detalhado de kill
struct s_arena7x7_kill_log
{
	uint32 killer_id;
	std::string killer_name;
	uint16 killer_class;
	uint32 victim_id;
	std::string victim_name;
	uint16 victim_class;
	uint16 skill_id;
	std::string skill_name;
	uint32 final_damage;
	uint32 assist1_id;
	std::string assist1_name;
	uint32 assist2_id;
	std::string assist2_name;
	t_tick timestamp;
};

/// Log de ataque normal (agregado por attacker->target)
struct s_arena7x7_attack_log
{
	uint32 attacker_id;
	std::string attacker_name;
	uint32 target_id;
	std::string target_name;
	int64 total_damage;
	uint32 hit_count;
	uint32 critical_count;
	uint32 miss_count;
};

/// Estrutura principal de uma partida
struct s_arena7x7_match
{
	uint32 match_id;
	std::string map_name;
	e_arena7x7_match_status status;
	e_arena7x7_team winner_team;
	uint16 blue_score;
	uint16 red_score;
	uint16 season;

	// Nomes das guilds dos times
	std::string blue_guild_name;
	std::string red_guild_name;
	uint32 blue_guild_id;
	uint32 red_guild_id;

	// Contagem de jogadores vivos em cada time
	uint16 blue_alive;
	uint16 red_alive;

	t_tick start_time;
	t_tick end_time;
	uint32 duration_seconds; // Dura��o da partida em segundos

	// Jogadores e estat�sticas
	std::unordered_map<uint32, std::shared_ptr<s_arena7x7_player_stats>> players; // char_id -> stats

	// Logs detalhados (para salvar no SQL no final)
	std::vector<s_arena7x7_damage_entry> damage_log;
	std::vector<s_arena7x7_support_entry> support_log;
	std::vector<s_arena7x7_item_entry> item_log;
	std::vector<s_arena7x7_kill_entry> kill_log;

	// Novos logs detalhados para o site
	std::vector<s_arena7x7_skill_log> skill_log;
	std::vector<s_arena7x7_item_log> item_usage_log;
	std::vector<s_arena7x7_kill_log> kill_detail_log;
	std::unordered_map<uint64, s_arena7x7_attack_log> attack_log; // (attacker_id << 32 | target_id) -> attack data

	// Dados agregados (atualizados em tempo real)
	std::unordered_map<uint64, s_arena7x7_damage_by_skill> damage_by_skill;	  // (char_id << 32 | skill_id) -> data
	std::unordered_map<uint64, s_arena7x7_damage_by_target> damage_by_target; // (attacker_id << 32 | target_id) -> data
	std::unordered_map<uint64, s_arena7x7_support_by_target> support_by_target;

	s_arena7x7_match() : match_id(0), status(ARENA7X7_MATCH_WAITING),
						 winner_team(ARENA7X7_TEAM_NONE), blue_score(0), red_score(0), season(1),
						 blue_guild_name("Time Azul"), red_guild_name("Time Vermelho"),
						 blue_guild_id(0), red_guild_id(0), blue_alive(0), red_alive(0),
						 start_time(0), end_time(0), duration_seconds(0) {}
};

// ============================================================================
// Vari�veis Globais
// ============================================================================

/// Mapa de partidas ativas (match_id -> match data)
extern std::unordered_map<uint32, std::shared_ptr<s_arena7x7_match>> arena7x7_matches;

/// Mapa de jogadores em partida (char_id -> match_id)
extern std::unordered_map<uint32, uint32> arena7x7_player_match;

/// Contador de match_id (ser� sincronizado com SQL)
extern uint32 arena7x7_match_counter;

// ============================================================================
// Fun��es de Gerenciamento de Partida
// ============================================================================

/**
 * Cria uma nova partida
 * @param map_name Nome do mapa
 * @return match_id da nova partida, ou 0 se falhar
 */
uint32 arena7x7_create_match(const char *map_name);

/**
 * Adiciona um jogador � partida
 * @param match_id ID da partida
 * @param sd Session data do jogador
 * @param team Time do jogador
 * @param is_leader Se � l�der do time
 * @return true se adicionado com sucesso
 */
bool arena7x7_add_player(uint32 match_id, map_session_data *sd, e_arena7x7_team team, bool is_leader = false);

/**
 * Define os nomes das guilds dos times
 * @param match_id ID da partida
 * @param blue_guild_name Nome da guild do time azul
 * @param red_guild_name Nome da guild do time vermelho
 * @param blue_guild_id ID da guild do time azul (opcional)
 * @param red_guild_id ID da guild do time vermelho (opcional)
 * @return true se definido com sucesso
 */
bool arena7x7_set_guild_names(uint32 match_id, const char *blue_guild_name, const char *red_guild_name, uint32 blue_guild_id = 0, uint32 red_guild_id = 0);

/**
 * Remove um jogador da partida
 * @param sd Session data do jogador
 * @param deserter Se saiu como desertor
 * @return true se removido com sucesso
 */
bool arena7x7_remove_player(map_session_data *sd, bool deserter = false);

/**
 * Inicia uma partida
 * @param match_id ID da partida
 * @return true se iniciada com sucesso
 */
bool arena7x7_start_match(uint32 match_id);

/**
 * Finaliza uma partida
 * @param match_id ID da partida
 * @param winner_team Time vencedor (0 = empate)
 * @param blue_score Pontua��o do time azul
 * @param red_score Pontua��o do time vermelho
 * @return true se finalizada com sucesso
 */
bool arena7x7_finish_match(uint32 match_id, e_arena7x7_team winner_team, uint16 blue_score, uint16 red_score);

/**
 * Cancela uma partida
 * @param match_id ID da partida
 * @return true se cancelada com sucesso
 */
bool arena7x7_cancel_match(uint32 match_id);

/**
 * Obt�m a partida de um jogador
 * @param char_id ID do personagem
 * @return Ponteiro para a partida ou nullptr
 */
std::shared_ptr<s_arena7x7_match> arena7x7_get_player_match(uint32 char_id);

/**
 * Verifica se um jogador est� em uma partida ativa
 * @param char_id ID do personagem
 * @return true se estiver em partida ativa
 */
bool arena7x7_is_in_match(uint32 char_id);

/**
 * Verifica se um jogador est� "morto" (tumba) na arena
 * @param char_id ID do personagem
 * @return true se estiver morto/tumba na arena
 */
bool arena7x7_is_player_dead(uint32 char_id);

/**
 * Obt�m uma partida pelo ID
 * @param match_id ID da partida
 * @return Ponteiro para a partida ou nullptr
 */
std::shared_ptr<s_arena7x7_match> arena7x7_get_match(uint32 match_id);

// ============================================================================
// Fun��es de Tracking de Estat�sticas
// ============================================================================

/**
 * Registra dano causado
 * @param match Partida
 * @param attacker_id ID do atacante
 * @param target_id ID do alvo
 * @param skill_id ID da skill (0 = ataque normal)
 * @param damage Quantidade de dano
 * @param is_critical Se foi cr�tico
 */
void arena7x7_record_damage(std::shared_ptr<s_arena7x7_match> match,
							uint32 attacker_id, uint32 target_id, uint16 skill_id, uint32 damage, bool is_critical);

/**
 * Registra uma kill
 * @param match Partida
 * @param killer_id ID do matador
 * @param victim_id ID da v�tima
 * @param skill_id Skill que causou a kill
 * @param kill_damage Dano final que causou a morte
 */
void arena7x7_record_kill(std::shared_ptr<s_arena7x7_match> match,
						  uint32 killer_id, uint32 victim_id, uint16 skill_id, uint32 kill_damage);

/**
 * Registra skill de suporte
 * @param match Partida
 * @param caster_id ID do caster
 * @param target_id ID do alvo
 * @param skill_id ID da skill
 * @param type Tipo de suporte
 * @param value Valor (HP curado, etc)
 */
void arena7x7_record_support(std::shared_ptr<s_arena7x7_match> match,
							 uint32 caster_id, uint32 target_id, uint16 skill_id, e_arena7x7_support_type type, uint32 value);

/**
 * Registra uso de item
 * @param match Partida
 * @param char_id ID do usu�rio
 * @param target_id ID do alvo (0 = si mesmo)
 * @param item_id ID do item
 * @param item_name Nome do item
 * @param type Tipo de item
 * @param value Valor recuperado
 */
void arena7x7_record_item_use(std::shared_ptr<s_arena7x7_match> match,
							  uint32 char_id, uint32 target_id, t_itemid item_id, const char *item_name,
							  e_arena7x7_item_type type, uint32 value);

/**
 * Registra consumo de SP
 * @param match Partida
 * @param char_id ID do jogador
 * @param sp_amount Quantidade de SP
 */
void arena7x7_record_sp_use(std::shared_ptr<s_arena7x7_match> match, uint32 char_id, uint32 sp_amount);

/**
 * Registra consumo de Zeny
 * @param match Partida
 * @param char_id ID do jogador
 * @param zeny_amount Quantidade de Zeny
 */
void arena7x7_record_zeny_use(std::shared_ptr<s_arena7x7_match> match, uint32 char_id, uint32 zeny_amount);

// ============================================================================
// Fun��es de Persist�ncia SQL
// ============================================================================

/**
 * Salva uma partida completa no banco de dados
 * @param match Partida a ser salva
 * @return true se salva com sucesso
 */
bool arena7x7_save_match(std::shared_ptr<s_arena7x7_match> match);

/**
 * Atualiza o ranking acumulado de um jogador
 * @param char_id ID do personagem
 * @param stats Estat�sticas da partida
 * @param won Se venceu a partida
 * @param lost Se perdeu a partida
 * @param tie Se empatou
 * @param count_match Se deve incrementar o contador de partidas jogadas (false para partidas canceladas)
 * @return true se atualizado com sucesso
 */
bool arena7x7_update_ranking(uint32 char_id, s_arena7x7_player_stats *stats, bool won, bool lost, bool tie, bool count_match = true);

/**
 * Carrega o pr�ximo match_id do banco
 * @return Pr�ximo match_id dispon�vel
 */
uint32 arena7x7_load_next_match_id();

/**
 * Carrega a temporada atual
 * @return ID da temporada ativa
 */
uint16 arena7x7_get_current_season();

// ============================================================================
// Fun��es de Utilidade
// ============================================================================

/**
 * Verifica se um jogador est� em uma partida ativa
 * @param char_id ID do personagem
 * @return true se est� em partida
 */
bool arena7x7_is_player_in_match(uint32 char_id);

/**
 * Obt�m o time de um jogador
 * @param char_id ID do personagem
 * @return Time do jogador ou NONE se n�o est� em partida
 */
e_arena7x7_team arena7x7_get_player_team(uint32 char_id);

/**
 * Verifica se dois jogadores s�o do mesmo time
 * @param char_id1 ID do primeiro jogador
 * @param char_id2 ID do segundo jogador
 * @return true se s�o do mesmo time
 */
bool arena7x7_same_team(uint32 char_id1, uint32 char_id2);

/**
 * Verifica se um jogador est� morto (tumba) na arena
 * @param char_id ID do personagem
 * @return true se est� morto
 */
bool arena7x7_is_player_dead(uint32 char_id);

/**
 * Atualiza o placar do BG (contador de jogadores vivos)
 * @param match Partida para atualizar
 */
void arena7x7_update_score_display(std::shared_ptr<s_arena7x7_match> match);

/**
 * Conta jogadores vivos em cada time
 * @param match Partida para contar
 */
void arena7x7_count_alive_players(std::shared_ptr<s_arena7x7_match> match);

/**
 * Transforma jogador em tumba (morte permanente na arena)
 * @param sd Session data do jogador
 */
void arena7x7_transform_to_tombstone(map_session_data *sd);

/**
 * Remove status de tumba e restaura jogador
 * @param sd Session data do jogador
 */
void arena7x7_restore_from_tombstone(map_session_data *sd);

/**
 * Restaura todos os t�mulos de uma partida
 * @param match_id ID da partida
 */
void arena7x7_restore_all_tombstones(uint32 match_id);

/**
 * Obt�m estat�sticas de um jogador na partida atual
 * @param char_id ID do personagem
 * @return Ponteiro para as estat�sticas ou nullptr
 */
std::shared_ptr<s_arena7x7_player_stats> arena7x7_get_player_stats(uint32 char_id);

// ============================================================================
// Hooks para integra��o com o c�digo existente
// ============================================================================

/**
 * Hook chamado quando ocorre dano
 * Deve ser chamado em status_damage()
 */
void arena7x7_on_damage(struct block_list *src, struct block_list *target,
						uint16 skill_id, int damage, bool is_critical);

/**
 * Hook chamado quando um jogador morre
 * Deve ser chamado em pc_dead()
 */
void arena7x7_on_death(map_session_data *killer, map_session_data *victim, uint16 skill_id);

/**
 * Hook chamado quando ocorre healing
 * Deve ser chamado nas skills de heal
 */
void arena7x7_on_heal(map_session_data *caster, map_session_data *target, uint16 skill_id, int heal_amount);

/**
 * Hook chamado quando um buff � aplicado
 */
void arena7x7_on_buff(map_session_data *caster, map_session_data *target, uint16 skill_id);

/**
 * Hook chamado quando um item � usado
 * Deve ser chamado em pc_useitem()
 */
void arena7x7_on_item_use(map_session_data *sd, map_session_data *target,
						  t_itemid item_id, const char *item_name, int value);

/**
 * Rastreia itens consumidos durante a partida (chamado em pc_delitem)
 * Monitora uma lista espec�fica de itens importantes para PvP
 */
void arena7x7_track_item_consume(map_session_data *sd, t_itemid item_id, const char *item_name, int amount);

/**
 * Hook chamado quando SP � consumido
 */
void arena7x7_on_sp_consume(map_session_data *sd, int sp_amount);

/**
 * Hook para logar uso detalhado de skill com dano
 * Chamado em skill.cpp quando uma skill causa dano
 */
void arena7x7_log_skill_damage(map_session_data *caster, struct block_list *target,
							   uint16 skill_id, uint16 skill_lv, int64 damage, int hit_count, bool is_critical);

/**
 * Hook para contar uso de skill (todas as skills)
 * Chamado em skill_consume_requirement quando uma skill � executada
 */
void arena7x7_count_skill_use(map_session_data *sd, uint16 skill_id);

/**
 * Hook para logar consumo de itens por skills (flechas, gemas, catalisadores)
 * Chamado em skill.cpp quando uma skill consome itens
 */
void arena7x7_log_skill_item_consumption(map_session_data *sd, t_itemid item_id,
										 uint16 amount, uint16 skill_id, const char *item_type);

/**
 * Hook para logar uso de flechas em ataques normais
 * Chamado em battle.cpp
 */
void arena7x7_log_arrow_consumption(map_session_data *sd, t_itemid arrow_id, uint16 amount, int32 skill_id);

/**
 * Hook para logar ataque normal detalhado
 */
void arena7x7_log_normal_attack(map_session_data *attacker, struct block_list *target,
								int64 damage, bool is_critical, bool is_miss);

/**
 * Salvar logs detalhados no banco de dados
 */
void arena7x7_save_detailed_logs(uint32 match_id);

// ============================================================================
// Inicializa��o e Finaliza��o
// ============================================================================

void do_init_arena7x7(void);
void do_final_arena7x7(void);

#endif /* ARENA7X7_HPP */
