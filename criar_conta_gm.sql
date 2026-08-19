USE principal;
INSERT INTO login (userid, user_pass, sex, group_id)
VALUES ('admin', 'admin', 'M', 99);
-- group_id 99 = "Admin" (confirmado em conf/groups.yml)
-- Troque 'meuadmin' e 'minhasenha' antes de rodar.
