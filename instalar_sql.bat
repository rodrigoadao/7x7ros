@echo off
REM Roda a partir da raiz do projeto 7x7ros. Pede a senha do usuario ragnarok a cada import.
SET MYSQL="C:\Program Files\MySQL\MySQL Server 26.7\bin\mysql.exe"

echo Criando bancos e usuario...
%MYSQL% -u root -p -e "CREATE DATABASE IF NOT EXISTS principal; CREATE DATABASE IF NOT EXISTS logar; CREATE DATABASE IF NOT EXISTS web; CREATE USER IF NOT EXISTS 'ragnarok'@'localhost' IDENTIFIED BY 'ragnarok'; GRANT ALL PRIVILEGES ON principal.* TO 'ragnarok'@'localhost'; GRANT ALL PRIVILEGES ON logar.* TO 'ragnarok'@'localhost'; GRANT ALL PRIVILEGES ON web.* TO 'ragnarok'@'localhost'; FLUSH PRIVILEGES;"

echo Importando tabelas principais...
%MYSQL% -u ragnarok -p principal < sql-files\main.sql
%MYSQL% -u ragnarok -p principal < sql-files\web.sql
%MYSQL% -u ragnarok -p principal < sql-files\roulette_default_data.sql
%MYSQL% -u ragnarok -p logar < sql-files\logs.sql

echo Criando conta GM...
%MYSQL% -u ragnarok -p principal < criar_conta_gm.sql

echo Concluido. Confira acima se algum comando deu erro.
pause
