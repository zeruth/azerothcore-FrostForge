--
-- The 3.4.3 world connection is keyed by Battle.net, not SRP6: at RealmJoin the Battle.net
-- service hands the client a 32-byte JoinSecret, and the world server derives the session from
-- [JoinSecret || the client's 32-byte secret]. bnet_join_key holds those 64 bytes from the join
-- until the world socket authenticates (world-handshake.ANALYSIS.md 3.3).
ALTER TABLE `account`
    ADD COLUMN `bnet_join_key` BINARY(64) NULL DEFAULT NULL AFTER `session_key`;
