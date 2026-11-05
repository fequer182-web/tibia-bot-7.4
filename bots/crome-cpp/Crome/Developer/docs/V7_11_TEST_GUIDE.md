# Crome v7.11 — Test guide

## 1. Profile Save / Load
1. Configure Cavebot waypoints, Targeting, Healing, Loot IDs, DropItem IDs, Walker IDs and Fishing.
2. Enter a profile name and click `Save Profile`.
3. Change some values.
4. Click `Load Profile`.
5. Confirm the UI itself returns to the saved values and the Cavebot waypoints are restored.

Profiles are saved in `Crome/profiles/user/<name>.json`.

## 2. Olders Look ID
1. Open Olders and Crome.
2. Confirm `Native: CONNECTED`.
3. Give Look on a ground item.
4. Footer should show `Look ID: <id> Pos: x,y,z`.
5. Give Look on an equipment/backpack item.
6. Footer should show `Look ID: <id>`.

Runtime path: `Game::detailedLook RVA 0x163B10`, `Thing* = *(RDX)`, ID `+0x38`.

## 3. Fishing
1. In Cavebot > IDs, confirm `Fishing Rod IDs` contains the real rod ID and `Fishing Water IDs` contains the real water ground IDs for that OT.
2. The rod may be equipped or inside an opened backpack.
3. Enable `Auto Fishing`.
4. Leave Cavebot and Targeting disabled during the isolated test.
5. Watch `CromeRuntime.log` for `[FISH]`, `[AUTO-GROUND]` and `[USEWITH-GROUND]` messages.
6. Each loop picks a valid water tile pseudo-randomly; when more than one is available it avoids selecting the same candidate twice in a row.

## 4. Follow / Keep Follow caution
Follow is useful only when intentionally chasing a selected creature/player. It is not required for normal Cavebot walking.
When `Keep Follow` is ON, movement can conflict conceptually with waypoint walking, so keep it OFF for normal route tests. Targeting/Walker arbitration remains enabled.

## 5. Loot / DropItem
`Loot IDs` and `DropItem IDs` are part of the saved Profile.
DropItem execution remains guarded until the build's native `Game::move` call is validated. The configuration is saved even when the native move action is unavailable.
