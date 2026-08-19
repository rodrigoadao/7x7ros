# Instalação do banco SQL para o 7x7ros

A wiki oficial (`SQL_Installation`) é de 2016 e usa MySQL 5.5 + Workbench antigo. Abaixo, o mesmo processo mas atualizado e já ajustado para os nomes reais configurados no seu `conf/inter_athena.conf`.

## O que eu conferi no seu projeto

- Servidor está em modo **Renewal** (`src/config/renewal.hpp` define `RENEWAL`).
- `conf/inter_athena.conf` já vem com credenciais padrão de desenvolvimento:
  - usuário/senha: `ragnarok` / `ragnarok`
  - banco principal (login+char+map): **`principal`** (não é "ragnarok" como a wiki antiga sugere)
  - banco de logs: **`logar`** (não é "log")
  - banco do web server: **`web`**
- `use_sql_db: no` → por padrão os itens/mobs vêm dos arquivos YAML em `db/`, não do SQL. Só precisa importar `item_db*.sql`/`mob_db*.sql` se você for mudar isso pra `yes`.
- `conf/import/inter_conf.txt` está vazio, ou seja, nada sobrescreve essas credenciais padrão.

## 1. Instalar o MySQL/MariaDB no Windows

Baixe o **MySQL Community Server** (https://dev.mysql.com/downloads/mysql/) ou **MariaDB** (https://mariadb.org/download/). Qualquer um funciona com rAthena. Durante a instalação, defina a senha do `root` e anote.

## 2. Criar os bancos e o usuário

Abra o "MySQL Command Line Client" (ou `mysql -u root -p` em qualquer terminal) e rode:

```sql
CREATE DATABASE principal;
CREATE DATABASE logar;
CREATE DATABASE web;

CREATE USER 'ragnarok'@'localhost' IDENTIFIED BY 'ragnarok';
GRANT ALL PRIVILEGES ON principal.* TO 'ragnarok'@'localhost';
GRANT ALL PRIVILEGES ON logar.* TO 'ragnarok'@'localhost';
GRANT ALL PRIVILEGES ON web.* TO 'ragnarok'@'localhost';
FLUSH PRIVILEGES;
```

(Se for só ambiente de teste local, pode pular a criação do usuário e importar tudo com `root` mesmo — os `.conf` já usam `ragnarok`/`ragnarok`, então se optar por isso ajuste `login_server_id`/`login_server_pw` etc. em `conf/inter_athena.conf` para `root`/sua senha.)

## 3. Importar as tabelas

A partir da raiz do projeto (`7x7ros/`), no PowerShell ou cmd:

```
mysql -u ragnarok -p principal < sql-files\main.sql
mysql -u ragnarok -p principal < sql-files\web.sql
mysql -u ragnarok -p principal < sql-files\roulette_default_data.sql
mysql -u ragnarok -p logar < sql-files\logs.sql
```

Isso já é suficiente para instalação nova — `main.sql` está atualizado com o schema atual, não precisa rodar nada da pasta `sql-files/upgrades/` (essa pasta é só para quem está *atualizando* uma instalação antiga).

### Opcional — só se for ligar `use_sql_db: yes`
Como o servidor está em Renewal, importe as versões `_re`:

```
mysql -u ragnarok -p principal < sql-files\item_db_re.sql
mysql -u ragnarok -p principal < sql-files\item_db2_re.sql
mysql -u ragnarok -p principal < sql-files\mob_db_re.sql
mysql -u ragnarok -p principal < sql-files\mob_db2_re.sql
mysql -u ragnarok -p principal < sql-files\mob_skill_db_re.sql
```

## 4. Criar sua conta GM

Depois de importar `main.sql`, insira uma conta direto na tabela `login` (schema `principal`):

```sql
USE principal;
INSERT INTO login (userid, user_pass, sex, group_id)
VALUES ('meuadmin', 'minhasenha', 'M', 99);
```

`group_id: 99` normalmente é GM máximo — confira `conf/groups.yml` pra confirmar o id certo no seu projeto.

## 5. Subir o servidor

Ordem obrigatória:

```
login-server.exe
char-server.exe
map-server.exe
```

Se algum deles fechar sozinho, o motivo aparece no console — normalmente é credencial/schema errado em `conf/inter_athena.conf`, `conf/login_athena.conf`, `conf/char_athena.conf` ou `conf/map_athena.conf`.

## Nota sobre a senha das tabelas de login

`convert_passwords.sql` (em `sql-files/tools/`) converte senhas de conta pra MD5 — não é necessário rodar isso, o rAthena aceita senha em texto puro por padrão em dev.
