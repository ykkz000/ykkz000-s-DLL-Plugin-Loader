-- Plugin declaration table for the timing probe (see the plan "dbapi-timing-probe").
-- The Loader's FrontEnd action creates the table first (LoadOrder 0); every enabled mod can then
-- insert its own plugin declaration (LoadOrder > 0), so the set of rows reflects the enabled mods.
-- This probe payload only carries the table itself plus one marker row for the Loader; it is
-- read-only from the Loader's side and changes no gameplay data.
CREATE TABLE IF NOT EXISTS YKKZ000_DLL_Plugins(
    ModId TEXT NOT NULL,
    Name TEXT NOT NULL,
    Dependencies TEXT,
    Dir TEXT,
    PRIMARY KEY(Name));

INSERT OR REPLACE INTO YKKZ000_DLL_Plugins(ModId, Name, Dependencies, Dir)
    VALUES('9e4ee2f0-c9d2-4eb4-8b38-b698411da023', 'ykkz000.loader.marker', '', '');
