
# Contents

## About MariaDB MaxScale

 - [About MariaDB MaxScale](About/About-MaxScale.md)
 - [Changelog](Changelog.md)
 - [Limitations](About/Limitations.md)

## Getting Started

 - [Building MariaDB MaxScale from Source Code](Getting-Started/Building-MaxScale-from-Source-Code.md)
 - [Configuration Guide](Getting-Started/Configuration-Guide.md)
 - [MariaDB MaxScale Installation Guide](Getting-Started/MariaDB-MaxScale-Installation-Guide.md)

## Upgrading MariaDB MaxScale

- [Upgrading MariaDB MaxScale from 2.2 to 2.3](Upgrading/Upgrading-To-MaxScale-2.3.md)
- [Upgrading MariaDB MaxScale from 2.1 to 2.2](Upgrading/Upgrading-To-MaxScale-2.2.md)
- [Upgrading MariaDB MaxScale from 2.0 to 2.1](Upgrading/Upgrading-To-MaxScale-2.1.md)
- [Upgrading MariaDB MaxScale from 1.4 to 2.0](Upgrading/Upgrading-To-MaxScale-2.0.md)

## Reference

 - [MaxAdmin - Admin Interface](Reference/MaxAdmin.md)
 - [Routing Hints](Reference/Hint-Syntax.md)
 - [MaxBinlogCheck](Reference/MaxBinlogCheck.md)
 - [MaxScale REST API](REST-API/API.md)
 - [Module Commands](Reference/Module-Commands.md)

## Tutorials

The main tutorial for MariaDB MaxScale consist of setting up MariaDB MaxScale
for the environment you are using with either a connection-based or a
read/write-based configuration.

 - [MariaDB MaxScale Tutorial](Tutorials/MaxScale-Tutorial.md)

These tutorials are for specific use cases and module combinations.

 - [Administration Tutorial](Tutorials/Administration-Tutorial.md)
 - [Avro Router Tutorial](Tutorials/Avrorouter-Tutorial.md)
 - [Configuring Servers](Tutorials/Configuring-Servers.md)
 - [Configuring the Galera Monitor](Tutorials/Configuring-Galera-Monitor.md)
 - [Configuring the MariaDB Monitor](Tutorials/Configuring-MariaDB-Monitor.md)
 - [Configuring MaxAdmin](Tutorials/Configuring-MaxAdmin.md)
 - [Connection Routing Tutorial](Tutorials/Connection-Routing-Tutorial.md)
 - [Encrypting Passwords](Tutorials/Encrypting-Passwords.md)
 - [Filter Tutorial](Tutorials/Filter-Tutorial.md)
 - [MariaDB MaxScale Information Schema Tutorial](Tutorials/MaxScale-Information-Schema.md)
 - [MariaDB Monitor Failover Tutorial](Tutorials/MariaDB-Monitor-Failover.md)
 - [MaxScale Failover with Keepalived and MaxCtrl](Tutorials/MaxScale-Failover-with-Keepalived-and-MaxCtrl.md)
 - [MySQL Cluster Setup](Tutorials/MySQL-Cluster-Setup.md)
 - [RabbitMQ Setup and MariaDB MaxScale Integration Tutorial](Tutorials/RabbitMQ-Setup-And-MaxScale-Integration.md)
 - [RabbitMQ and Tee Filter Data Archiving Tutorial](Tutorials/RabbitMQ-And-Tee-Archiving.md)
 - [Read Write Splitting Tutorial](Tutorials/Read-Write-Splitting-Tutorial.md)
 - [Replication Proxy with the Binlog Router Tutorial](Tutorials/Replication-Proxy-Binlog-Router-Tutorial.md)
 - [Simple Schema Sharding Tutorial](Tutorials/Simple-Sharding-Tutorial.md)

Here are tutorials on monitoring and managing MariaDB MaxScale in cluster
environments.

 - [MariaDB MaxScale HA with Lsyncd](Tutorials/MaxScale-HA-with-lsyncd.md)
 - [Nagios Plugins for MariaDB MaxScale Tutorial](Tutorials/Nagios-Plugins.md)

## Routers

The routing module is the core of a MariaDB MaxScale service. The router documentation
contains all module specific configuration options and detailed explanations
of their use.

 - [Avrorouter](Routers/Avrorouter.md)
 - [Binlogrouter](Routers/Binlogrouter.md)
 - [Cat](Routers/Cat.md)
 - [HintRouter](Routers/HintRouter.md)
 - [Read Connection Router](Routers/ReadConnRoute.md)
 - [Read Write Split](Routers/ReadWriteSplit.md)
 - [Schemarouter](Routers/SchemaRouter.md)

There are also two diagnostic routing modules. The CLI is for MaxAdmin and
the Debug CLI client for Telnet.

 - [CLI](Routers/CLI.md)

## Filters

Here are detailed documents about the filters MariaDB MaxScale offers. They
contain configuration guides and example use cases. Before reading these, you
should have read the filter tutorial so that you know how they work and how to
configure them.

 - [Binlog Filter](Filters/BinlogFilter.md)
 - [Cache](Filters/Cache.md)
 - [Comment Filter](Filters/Comment.md)
 - [Consistent Critical Read Filter](Filters/CCRFilter.md)
 - [Database Firewall Filter](Filters/Database-Firewall-Filter.md)
 - [Insert Stream Filter](Filters/Insert-Stream-Filter.md)
 - [Luafilter](Filters/Luafilter.md)
 - [Masking Filter](Filters/Masking.md)
 - [Maxrows Filter](Filters/Maxrows.md)
 - [Named Server Filter](Filters/Named-Server-Filter.md)
 - [Query Log All](Filters/Query-Log-All-Filter.md)
 - [RabbitMQ Filter](Filters/RabbitMQ-Filter.md)
 - [Regex Filter](Filters/Regex-Filter.md)
 - [Tee Filter](Filters/Tee-Filter.md)
 - [Top N Filter](Filters/Top-N-Filter.md)
 - [Transaction Performance Monitoring Filter](Filters/Transaction-Performance-Monitoring-Filter.md)

## Monitors

Common options for all monitor modules.

 - [Monitor Common](Monitors/Monitor-Common.md)

Module specific documentation.

 - [Aurora Monitor](Monitors/Aurora-Monitor.md)
 - [Galera Monitor](Monitors/Galera-Monitor.md)
 - [MariaDB Monitor](Monitors/MariaDB-Monitor.md)
 - [Multi-Master Monitor](Monitors/MM-Monitor.md)
 - [MySQL Cluster Monitor](Monitors/NDB-Cluster-Monitor.md)

## Protocols

Documentation for MaxScale protocol modules.

 - [Change Data Capture (CDC) Protocol](Protocols/CDC.md)
 - [Change Data Capture (CDC) Users](Protocols/CDC_users.md)

The MaxScale CDC Connector provides a C++ API for consuming data from a CDC
system.

 - [CDC Connector](Connectors/CDC-Connector.md)

## Authenticators

A short description of the authentication module type can be found in the
[Authentication Modules](Authenticators/Authentication-Modules.md)
document.

 - [GSSAPI Authenticator](Authenticators/GSSAPI-Authenticator.md)
 - [MySQL Authenticator](Authenticators/MySQL-Authenticator.md)

## Utilities

 - [RabbitMQ Consumer Client](Filters/RabbitMQ-Consumer-Client.md)

## Design Documents

 - [Plugin development guide](Design-Documents/Plugin-development-guide.md)
