#!/bin/bash

mysql -u root --force $1 <<EOF >& /dev/null

DROP USER IF EXISTS '$node_user'@'%';
CREATE USER '$node_user'@'%' IDENTIFIED BY '$node_password';
GRANT ALL PRIVILEGES ON *.* TO '$node_user'@'%' WITH GRANT OPTION;

DROP USER IF EXISTS 'repl'@'%';
CREATE USER 'repl'@'%' IDENTIFIED BY 'repl';
GRANT ALL ON *.* TO 'repl'@'%' WITH GRANT OPTION;

DROP USER IF EXISTS 'repl'@'localhost';
CREATE USER 'repl'@'localhost' IDENTIFIED BY 'repl';
GRANT ALL ON *.* TO 'repl'@'localhost' WITH GRANT OPTION;

DROP USER IF EXISTS 'skysql'@'%';
CREATE USER 'skysql'@'%' IDENTIFIED BY 'skysql';
GRANT ALL ON *.* TO 'skysql'@'%' WITH GRANT OPTION;

DROP USER IF EXISTS 'skysql'@'localhost';
CREATE USER 'skysql'@'localhost' IDENTIFIED BY 'skysql';
GRANT ALL ON *.* TO 'skysql'@'localhost' WITH GRANT OPTION;

DROP USER IF EXISTS 'maxskysql'@'%';
CREATE USER 'maxskysql'@'%' IDENTIFIED BY 'skysql';
GRANT ALL ON *.* TO 'maxskysql'@'%' WITH GRANT OPTION;

DROP USER IF EXISTS 'maxskysql'@'localhost';
CREATE USER 'maxskysql'@'localhost' IDENTIFIED BY 'skysql';
GRANT ALL ON *.* TO 'maxskysql'@'localhost' WITH GRANT OPTION;

DROP USER IF EXISTS 'maxuser'@'%';
CREATE USER 'maxuser'@'%' IDENTIFIED BY 'maxpwd';
GRANT ALL ON *.* TO 'maxuser'@'%' WITH GRANT OPTION;

DROP USER IF EXISTS 'maxuser'@'localhost';
CREATE USER 'maxuser'@'localhost' IDENTIFIED BY 'maxpwd';
GRANT ALL ON *.* TO 'maxuser'@'localhost' WITH GRANT OPTION;

RESET MASTER;
EOF
