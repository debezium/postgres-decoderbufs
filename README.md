[![License](https://img.shields.io/badge/license-MIT-brightgreen.svg?maxAge=2592000)](https://opensource.org/licenses/MIT)
[![Version](https://img.shields.io/badge/version-0.1.0-brightgreen.svg?maxAge=2592000)]()
[![User chat](https://img.shields.io/badge/chat-users-brightgreen.svg)](https://gitter.im/debezium/user)
[![Developer chat](https://img.shields.io/badge/chat-devs-brightgreen.svg)](https://gitter.im/debezium/dev)
[![Google Group](https://img.shields.io/:mailing%20list-debezium-brightgreen.svg)](https://groups.google.com/forum/#!forum/debezium)
[![Stack Overflow](http://img.shields.io/:stack%20overflow-debezium-brightgreen.svg)](http://stackoverflow.com/questions/tagged/debezium)

# Postgres Decoderbufs

A PostgreSQL logical decoder output plugin to deliver data as [Protocol Buffers](https://developers.google.com/protocol-buffers), adapted for Debezium

## Thanks to
- The original [Decoderbufs Project](https://github.com/xstevens/decoderbufs) on which this is based 
- [The PostgreSQL Team](https://postgresql.org) for adding [logical decoding](http://www.postgresql.org/docs/9.4/static/logicaldecoding.html) support

## Dependencies
This code depends on the following libraries and requires them for compilation:

* [PostgreSQL](http://www.postgresql.org) 9.6+
* [Protobuf-c](https://github.com/protobuf-c/protobuf-c) 1.2+ - used for data serialization
* [PostGIS](http://www.postgis.net/) 2.1+ - used for Postgres geometric types support

## Building

`postgres-decoderbufs` has to be built from source after installing required dependencies.  The required dependencies are first PostgreSQL
(for pg_config), PostgreSQL server development packages, protobuf-c for the Protocol Buffer support and some PostGIS development packages.

### Installing Dependencies
#### Debian

    # Core build utilities 
    apt-get update && apt-get install -f -y software-properties-common build-essential pkg-config git postgresql-server-dev-9.6 
    
    # PostGIS dependency
    apt-get install -f -y libproj-dev liblwgeom-dev

    # Protobuf-c dependency (requires a non-stable Debian repo)
    add-apt-repository "deb http://ftp.debian.org/debian testing main contrib" && apt-get update
    apt-get install -y libprotobuf-c-dev=1.2.1-1+b1

The above are taken from the Debezium [docker images](https://github.com/debezium/docker-images).

#### Other Linux distributions

You just need to make sure the above software packages (_or some flavour thereof_) are installed for your distro. 
Note that the last step from the above sequence is only required for Debian to be able to install `libprotobuf-c-dev:1.2.1`

### Building and installing decoderbufs

If you have all of the above prerequisites installed, clone this git repo to build from source.  If you have multiple
postgres versions installed, you can select which version to install by altering your `$PATH` to point to the right version.
Then `make` and `make install` for each version.  Here is an example:

    git clone https://github.com/debezium/postgres-decoderbufs.git
    cd postgres-decoderbufs
    # Install for Postgres 9.6 if I have multiple local versions
    export PATH=/usr/lib/postgresql/9.6/bin:$PATH
    make
    make install

Once the extension has been installed you just need to enable it and logical replication in postgresql.conf:

    # MODULES
    shared_preload_libraries = 'decoderbufs'
    
    # REPLICATION
    wal_level = logical             # minimal, archive, hot_standby, or logical (change requires restart)
    max_wal_senders = 8             # max number of walsender processes (change requires restart)
    wal_keep_segments = 4           # in logfile segments, 16MB each; 0 disables
    #wal_sender_timeout = 60s       # in milliseconds; 0 disables
    max_replication_slots = 4       # max number of replication slots (change requires restart)

In addition, permissions will have to be added for the user that connects to the DB to be able to replicate. This can be modified in _pg\_hba.conf_ like so:

    local   replication     <youruser>                          trust
    host    replication     <youruser>  127.0.0.1/32            trust
    host    replication     <youruser>  ::1/128                 trust
    
And restart PostgreSQL.

## Usage
    -- can use SQL for demo purposes
    select * from pg_create_logical_replication_slot('decoderbufs_demo', 'decoderbufs');
    
    -- DO SOME TABLE MODIFICATIONS (see below about UPDATE/DELETE)
    
    -- peek at WAL changes using decoderbufs debug mode for SQL console
    select data from pg_logical_slot_peek_changes('decoderbufs_demo', NULL, NULL, 'debug-mode', '1');
    -- get WAL changes using decoderbufs to update the WAL position
    select data from pg_logical_slot_get_changes('decoderbufs_demo', NULL, NULL, 'debug-mode', '1');
    
    -- check the WAL position of logical replicators
    select * from pg_replication_slots where slot_type = 'logical';

If you're performing an UPDATE/DELETE on your table and you don't see results for those operations from logical decoding, make sure you have set [REPLICA IDENTITY](http://www.postgresql.org/docs/9.4/static/sql-altertable.html#SQL-CREATETABLE-REPLICA-IDENTITY) appropriately for your use case.
    
The binary format will be consumed by the Debezium Postgres Connector.
          
## Type Mappings

The following table shows how current PostgreSQL type OIDs are mapped to which decoderbuf fields:

| PostgreSQL Type OID | Decoderbuf Field |
|---------------------|---------------|
| BOOLOID             | datum_boolean |
| INT2OID             | datum_int32   |
| INT4OID             | datum_int32   |
| INT8OID             | datum_int64   |
| OIDOID              | datum_int64   |
| FLOAT4OID           | datum_float   |
| FLOAT8OID           | datum_double  |
| NUMERICOID          | datum_double  |
| CHAROID             | datum_string  |
| VARCHAROID          | datum_string  |
| BPCHAROID           | datum_string  |
| TEXTOID             | datum_string  |
| JSONOID             | datum_string  |
| XMLOID              | datum_string  |
| UUIDOID             | datum_string  |
| TIMESTAMPOID        | datum_string  |
| TIMESTAMPTZOID      | datum_string  |
| BYTEAOID            | datum_bytes   |
| POINTOID            | datum_point   |
| PostGIS geometry    | datum_point   |
| PostGIS geography   | datum_point   |

## Support

File bug reports and feature requests using [Debezium's JIRA](https://issues.jboss.org/browse/DBZ) and the 
[postgresql-connector](https://issues.jboss.org/browse/DBZ/component/12323543) component
