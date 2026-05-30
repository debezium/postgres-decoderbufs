select slot_name from pg_create_logical_replication_slot('decoderbufs_demo', 'decoderbufs');

create table foo (id int primary key, data text);
insert into foo values (1, 'Important stuff');

select regexp_replace(data, '(txid|commit_time)\[\d+\]', '\1[***]', 'g') as data
from pg_logical_slot_peek_changes('decoderbufs_demo', NULL, NULL, 'debug-mode', '1');
