CREATE TABLE IF NOT EXISTS `arena7x7_matches` (
  `match_id` int(11) unsigned NOT NULL,
  `season` smallint(5) unsigned NOT NULL DEFAULT 1,
  `blue_guild_id` int(11) unsigned NOT NULL DEFAULT 0,
  `blue_guild_name` varchar(24) NOT NULL DEFAULT '',
  `red_guild_id` int(11) unsigned NOT NULL DEFAULT 0,
  `red_guild_name` varchar(24) NOT NULL DEFAULT '',
  `map_name` varchar(24) NOT NULL DEFAULT '',
  `start_time` datetime NOT NULL,
  `end_time` datetime NOT NULL,
  `duration_seconds` int(11) unsigned NOT NULL DEFAULT 0,
  `winner_team` tinyint(3) unsigned NOT NULL DEFAULT 0,
  `blue_score` smallint(5) unsigned NOT NULL DEFAULT 0,
  `red_score` smallint(5) unsigned NOT NULL DEFAULT 0,
  `status` tinyint(3) unsigned NOT NULL DEFAULT 0,
  PRIMARY KEY (`match_id`)
) ENGINE=MyISAM;

CREATE TABLE IF NOT EXISTS `arena7x7_match_players` (
  `match_id` int(11) unsigned NOT NULL,
  `char_id` int(11) unsigned NOT NULL,
  `account_id` int(11) unsigned NOT NULL DEFAULT 0,
  `char_name` varchar(24) NOT NULL DEFAULT '',
  `team` tinyint(3) unsigned NOT NULL DEFAULT 0,
  `job_class` smallint(5) unsigned NOT NULL DEFAULT 0,
  `base_level` smallint(5) unsigned NOT NULL DEFAULT 0,
  `guild_id` int(11) unsigned NOT NULL DEFAULT 0,
  `guild_name` varchar(24) NOT NULL DEFAULT '',
  `is_leader` tinyint(1) unsigned NOT NULL DEFAULT 0,
  `is_deserter` tinyint(1) unsigned NOT NULL DEFAULT 0,
  `is_winner` tinyint(1) unsigned NOT NULL DEFAULT 0,
  PRIMARY KEY (`match_id`, `char_id`)
) ENGINE=MyISAM;

CREATE TABLE IF NOT EXISTS `arena7x7_match_stats` (
  `match_id` int(11) unsigned NOT NULL,
  `char_id` int(11) unsigned NOT NULL,
  `kills` int(11) unsigned NOT NULL DEFAULT 0,
  `deaths` int(11) unsigned NOT NULL DEFAULT 0,
  `assists` int(11) unsigned NOT NULL DEFAULT 0,
  `damage_done` bigint(20) unsigned NOT NULL DEFAULT 0,
  `damage_received` bigint(20) unsigned NOT NULL DEFAULT 0,
  `healing_done` bigint(20) unsigned NOT NULL DEFAULT 0,
  `healing_received` bigint(20) unsigned NOT NULL DEFAULT 0,
  `skills_used` int(11) unsigned NOT NULL DEFAULT 0,
  `items_used` int(11) unsigned NOT NULL DEFAULT 0,
  `top_damage` bigint(20) unsigned NOT NULL DEFAULT 0,
  `best_streak` int(11) unsigned NOT NULL DEFAULT 0,
  `support_skills` int(11) unsigned NOT NULL DEFAULT 0,
  `hp_potions` int(11) unsigned NOT NULL DEFAULT 0,
  `sp_potions` int(11) unsigned NOT NULL DEFAULT 0,
  `yellow_gems` int(11) unsigned NOT NULL DEFAULT 0,
  `red_gems` int(11) unsigned NOT NULL DEFAULT 0,
  `blue_gems` int(11) unsigned NOT NULL DEFAULT 0,
  `poison_bottles` int(11) unsigned NOT NULL DEFAULT 0,
  `sp_consumed` bigint(20) unsigned NOT NULL DEFAULT 0,
  `zeny_consumed` bigint(20) unsigned NOT NULL DEFAULT 0,
  `time_alive` bigint(20) unsigned NOT NULL DEFAULT 0,
  PRIMARY KEY (`match_id`, `char_id`)
) ENGINE=MyISAM;

CREATE TABLE IF NOT EXISTS `arena7x7_skill_summary` (
  `match_id` int(11) unsigned NOT NULL,
  `char_id` int(11) unsigned NOT NULL,
  `char_name` varchar(24) NOT NULL DEFAULT '',
  `skill_id` smallint(5) unsigned NOT NULL DEFAULT 0,
  `skill_name` varchar(100) NOT NULL DEFAULT '',
  `total_damage` bigint(20) unsigned NOT NULL DEFAULT 0,
  `total_hits` int(11) unsigned NOT NULL DEFAULT 0,
  `use_count` int(11) unsigned NOT NULL DEFAULT 0,
  `kills_with_skill` int(11) unsigned NOT NULL DEFAULT 0,
  PRIMARY KEY (`match_id`, `char_id`, `skill_id`)
) ENGINE=MyISAM;

CREATE TABLE IF NOT EXISTS `arena7x7_damage_log` (
  `id` bigint(20) unsigned NOT NULL AUTO_INCREMENT,
  `match_id` int(11) unsigned NOT NULL DEFAULT 0,
  `attacker_id` int(11) unsigned NOT NULL DEFAULT 0,
  `target_id` int(11) unsigned NOT NULL DEFAULT 0,
  `skill_id` smallint(5) unsigned NOT NULL DEFAULT 0,
  `damage` bigint(20) unsigned NOT NULL DEFAULT 0,
  `is_critical` tinyint(1) unsigned NOT NULL DEFAULT 0,
  `is_kill` tinyint(1) unsigned NOT NULL DEFAULT 0,
  PRIMARY KEY (`id`),
  KEY (`match_id`),
  KEY (`attacker_id`, `target_id`)
) ENGINE=MyISAM;

CREATE TABLE IF NOT EXISTS `arena7x7_damage_by_skill` (
  `match_id` int(11) unsigned NOT NULL,
  `char_id` int(11) unsigned NOT NULL,
  `skill_id` smallint(5) unsigned NOT NULL DEFAULT 0,
  `skill_name` varchar(100) NOT NULL DEFAULT '',
  `total_damage` bigint(20) unsigned NOT NULL DEFAULT 0,
  `hit_count` int(11) unsigned NOT NULL DEFAULT 0,
  `critical_count` int(11) unsigned NOT NULL DEFAULT 0,
  `kill_count` int(11) unsigned NOT NULL DEFAULT 0,
  `max_damage` bigint(20) unsigned NOT NULL DEFAULT 0,
  PRIMARY KEY (`match_id`, `char_id`, `skill_id`)
) ENGINE=MyISAM;

CREATE TABLE IF NOT EXISTS `arena7x7_support_log` (
  `id` bigint(20) unsigned NOT NULL AUTO_INCREMENT,
  `match_id` int(11) unsigned NOT NULL DEFAULT 0,
  `caster_id` int(11) unsigned NOT NULL DEFAULT 0,
  `target_id` int(11) unsigned NOT NULL DEFAULT 0,
  `skill_id` smallint(5) unsigned NOT NULL DEFAULT 0,
  `support_type` tinyint(3) unsigned NOT NULL DEFAULT 0,
  `value` bigint(20) unsigned NOT NULL DEFAULT 0,
  PRIMARY KEY (`id`),
  KEY (`match_id`),
  KEY (`caster_id`, `target_id`)
) ENGINE=MyISAM;

CREATE TABLE IF NOT EXISTS `arena7x7_support_by_target` (
  `match_id` int(11) unsigned NOT NULL,
  `caster_id` int(11) unsigned NOT NULL,
  `target_id` int(11) unsigned NOT NULL,
  `total_healing` bigint(20) unsigned NOT NULL DEFAULT 0,
  `buff_count` int(11) unsigned NOT NULL DEFAULT 0,
  `skill_count` int(11) unsigned NOT NULL DEFAULT 0,
  PRIMARY KEY (`match_id`, `caster_id`, `target_id`)
) ENGINE=MyISAM;

CREATE TABLE IF NOT EXISTS `arena7x7_item_usage` (
  `id` bigint(20) unsigned NOT NULL AUTO_INCREMENT,
  `match_id` int(11) unsigned NOT NULL DEFAULT 0,
  `char_id` int(11) unsigned NOT NULL DEFAULT 0,
  `target_id` int(11) unsigned NOT NULL DEFAULT 0,
  `item_id` int(11) unsigned NOT NULL DEFAULT 0,
  `item_name` varchar(100) NOT NULL DEFAULT '',
  `item_type` tinyint(3) unsigned NOT NULL DEFAULT 0,
  `value` bigint(20) unsigned NOT NULL DEFAULT 0,
  PRIMARY KEY (`id`),
  KEY (`match_id`),
  KEY (`char_id`)
) ENGINE=MyISAM;

CREATE TABLE IF NOT EXISTS `arena7x7_kills` (
  `id` bigint(20) unsigned NOT NULL AUTO_INCREMENT,
  `match_id` int(11) unsigned NOT NULL DEFAULT 0,
  `killer_id` int(11) unsigned NOT NULL DEFAULT 0,
  `victim_id` int(11) unsigned NOT NULL DEFAULT 0,
  `kill_skill_id` smallint(5) unsigned NOT NULL DEFAULT 0,
  `kill_damage` bigint(20) unsigned NOT NULL DEFAULT 0,
  `assist1_id` int(11) unsigned NOT NULL DEFAULT 0,
  `assist2_id` int(11) unsigned NOT NULL DEFAULT 0,
  `assist3_id` int(11) unsigned NOT NULL DEFAULT 0,
  `killer_streak` int(11) unsigned NOT NULL DEFAULT 0,
  PRIMARY KEY (`id`),
  KEY (`match_id`),
  KEY (`killer_id`),
  KEY (`victim_id`)
) ENGINE=MyISAM;

CREATE TABLE IF NOT EXISTS `arena7x7_skill_log` (
  `id` bigint(20) unsigned NOT NULL AUTO_INCREMENT,
  `match_id` int(11) unsigned NOT NULL DEFAULT 0,
  `timestamp` bigint(20) unsigned NOT NULL DEFAULT 0,
  `caster_id` int(11) unsigned NOT NULL DEFAULT 0,
  `caster_name` varchar(24) NOT NULL DEFAULT '',
  `target_id` int(11) unsigned NOT NULL DEFAULT 0,
  `target_name` varchar(24) NOT NULL DEFAULT '',
  `skill_id` smallint(5) unsigned NOT NULL DEFAULT 0,
  `skill_name` varchar(100) NOT NULL DEFAULT '',
  `skill_level` smallint(5) unsigned NOT NULL DEFAULT 0,
  `damage` bigint(20) unsigned NOT NULL DEFAULT 0,
  `hits` int(11) unsigned NOT NULL DEFAULT 0,
  `is_critical` tinyint(1) unsigned NOT NULL DEFAULT 0,
  `is_kill` tinyint(1) unsigned NOT NULL DEFAULT 0,
  PRIMARY KEY (`id`),
  KEY (`match_id`),
  KEY (`caster_id`, `target_id`)
) ENGINE=MyISAM;

CREATE TABLE IF NOT EXISTS `arena7x7_item_log` (
  `id` bigint(20) unsigned NOT NULL AUTO_INCREMENT,
  `match_id` int(11) unsigned NOT NULL DEFAULT 0,
  `timestamp` bigint(20) unsigned NOT NULL DEFAULT 0,
  `char_id` int(11) unsigned NOT NULL DEFAULT 0,
  `char_name` varchar(24) NOT NULL DEFAULT '',
  `item_id` int(11) unsigned NOT NULL DEFAULT 0,
  `item_name` varchar(100) NOT NULL DEFAULT '',
  `amount` int(11) unsigned NOT NULL DEFAULT 0,
  `item_type` varchar(50) NOT NULL DEFAULT '',
  `related_skill_id` int(11) unsigned NOT NULL DEFAULT 0,
  PRIMARY KEY (`id`),
  KEY (`match_id`),
  KEY (`char_id`)
) ENGINE=MyISAM;

CREATE TABLE IF NOT EXISTS `arena7x7_ranking` (
  `char_id` int(11) unsigned NOT NULL,
  `char_name` varchar(24) NOT NULL DEFAULT '',
  `account_id` int(11) unsigned NOT NULL DEFAULT 0,
  `wins` int(11) unsigned NOT NULL DEFAULT 0,
  `losses` int(11) unsigned NOT NULL DEFAULT 0,
  `ties` int(11) unsigned NOT NULL DEFAULT 0,
  `matches_played` int(11) unsigned NOT NULL DEFAULT 0,
  `total_kills` int(11) unsigned NOT NULL DEFAULT 0,
  `total_deaths` int(11) unsigned NOT NULL DEFAULT 0,
  `total_assists` int(11) unsigned NOT NULL DEFAULT 0,
  `total_damage_done` bigint(20) unsigned NOT NULL DEFAULT 0,
  `total_damage_received` bigint(20) unsigned NOT NULL DEFAULT 0,
  `total_healing_done` bigint(20) unsigned NOT NULL DEFAULT 0,
  `total_skills_used` int(11) unsigned NOT NULL DEFAULT 0,
  `total_items_used` int(11) unsigned NOT NULL DEFAULT 0,
  `points` int(11) NOT NULL DEFAULT 0,
  `season_points` int(11) NOT NULL DEFAULT 0,
  `best_damage_match` bigint(20) unsigned NOT NULL DEFAULT 0,
  `best_killstreak` int(11) unsigned NOT NULL DEFAULT 0,
  `best_kills_match` int(11) unsigned NOT NULL DEFAULT 0,
  `last_match` datetime DEFAULT NULL,
  PRIMARY KEY (`char_id`)
) ENGINE=MyISAM;

CREATE TABLE IF NOT EXISTS `arena7x7_ranking_by_class` (
  `char_id` int(11) unsigned NOT NULL,
  `class` smallint(5) unsigned NOT NULL DEFAULT 0,
  `matches_played` int(11) unsigned NOT NULL DEFAULT 0,
  `wins` int(11) unsigned NOT NULL DEFAULT 0,
  `total_kills` int(11) unsigned NOT NULL DEFAULT 0,
  `total_deaths` int(11) unsigned NOT NULL DEFAULT 0,
  `total_damage` bigint(20) unsigned NOT NULL DEFAULT 0,
  `total_healing` bigint(20) unsigned NOT NULL DEFAULT 0,
  PRIMARY KEY (`char_id`, `class`)
) ENGINE=MyISAM;