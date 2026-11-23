-- The country a request came from is no longer kept: its column goes, with what it held.
ALTER TABLE sessions DROP COLUMN country;
ALTER TABLE feedback DROP COLUMN country;
-- The port's patch sets that were on (their names, comma separated) and the number of mods: how many were on, and
-- how many of those change the game's code.
ALTER TABLE sessions ADD COLUMN patches TEXT;
ALTER TABLE sessions ADD COLUMN mods_count INTEGER;
ALTER TABLE sessions ADD COLUMN mods_code INTEGER;
