-- Replace the GameCore DLL with the ykkz000 DLL plugin loader for Gathering Storm (Expansion2)
UPDATE GameCores
SET PackageId = '9e4ee2f0-c9d2-4eb4-8b38-b698411da023',
    DllPrefix = 'GameCore_YKKZ000_Loader_XP2'
WHERE GameCore = 'Expansion2';
