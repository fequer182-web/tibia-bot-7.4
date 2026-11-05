# Current baseline — Ramonia x86

## Probe

Version: **V37**

Canonical source:

`src/RamoniaResearchProbe.cpp`

The source was previously stored under the misleading historical name:

`OTCLab_BuildMigrator_V4_9_DIRECT_BIND_x86.cpp`

No functional code change was made during the repository consolidation.

## Canonical profile

`profiles/ramonia_x86/6AB03F94_E281E9CA5BA0AF2.json`

Confirmed core values:

- Game owner RVA: `0xA2D4D0`
- Map owner RVA: `0xA2D600`
- autoWalk: `0x76860`
- attack: `0x79190`
- follow: `0x79300`
- look: `0x77640`
- move: `0x77920`
- use: `0x780D0`
- useWith: `0x784F0`
- talk: `0x796F0`
- talkChannel: `0x79750`

Tile::things:

- begin `+0x24`
- end `+0x28`
- cap `+0x2C`
- stride `4`
- Tile vtable RVA `0x774208`

## V33.1 runtime capture safety + V37 ABI final

- target page protection is changed before arming;
- VEH does not call `VirtualProtect`;
- original byte is restored immediately on breakpoint hit;
- EIP is reset to the restored first instruction;
- diagnostic memory snapshots use `ReadProcessMemory`;
- protection restoration happens outside the VEH;
- breakpoint is one-shot and is not re-armed automatically.

## Rule

The profile is the source of truth for already-confirmed offsets/RVAs.
V34/V37 promote only classified runtime evidence into the portable `actions` profile block. The research probe remains non-calling; active calls now live in the separate Crome pipe bridge/runtime.


## V37 final contract

Already confirmed from V34 profile: `attack`, `follow`, `talkChannel`.

V37 finalizes the remaining move ABI: `autoWalk`, `look`, `move`, `use`, `useWith`. It validates direction-vector contents, stack-local shared-object holders and OTC Position encoding before promoting an action to `runtime_confirmed`.

The profile schema emitted by V35 is `otclab-ramonia-x86-research-v37` / `profileVersion: 37`.


V36 narrows the remaining ABI gap to `move.arg2`, using LocalPlayer-relative Position evidence and nested snapshots before promotion.


## V37 final contract

All primary actions used by the planned Crome runtime are now represented as `runtime_confirmed` in the canonical Ramonia profile.

The last gap, `move.arg2`, was closed from V36 runtime bytes as a direct Position with `u32 x + u32 y + u8 z`, validated against the live LocalPlayer position.

`useWith.arg2` is exposed semantically as generic `Thing` because runtime captures have shown different concrete RTTI targets while preserving the same shared-object holder ABI.


## Runtime bridge validation

The Ramonia pipe bridge is now beyond read-only proof-of-concept.

Confirmed end-to-end path:

- external x64 client -> named pipe -> injected x86 bridge;
- build/profile fingerprint match before enabling actions;
- WndProc dispatcher executes calls on the game/window thread;
- `Game::attack` at RVA `0x79190` confirmed with:
  - `ECX = Game*`
  - arg1 = `Creature*`
  - arg2 = `bool Cancel`
  - `Cancel=false` starts real combat and produces damage.

The canonical profile records the second bool argument. The V37 ABI capture now inspects two attack arguments and persists the observed bool value for future builds.

Bridge V5 adds:

- `GET_VISIBLE_MONSTERS`
- `ATTACK_ID`

`ATTACK_ID` keeps raw process pointers inside the x86 bridge. The external Crome side sends only a semantic `creatureId`.


## ATTACK_ID runtime-confirmed

Bridge V5 was validated end-to-end on the live Ramonia client:

- `GET_VISIBLE_MONSTERS` returned Monster ID + position without Follow/manual target;
- `ATTACK_ID <creatureId>` resolved the Monster pointer inside the x86 bridge;
- before the action both AttackTarget and FollowTarget were empty;
- after the action AttackTarget pointed to the requested Monster ID;
- the call executed through the game-thread dispatcher.

This closes the first semantic Crome action path without raw pointers crossing IPC:

`Crome x64 -> creatureId -> pipe -> x86 resolver -> Game::attack(Creature*, false)`.


## Automatic target loop

The x64 pipe test now contains the first external target-policy loop. It polls semantic monster snapshots, keeps the current matching target while alive, selects the nearest matching monster when needed, and invokes `ATTACK_ID`. This keeps target policy in the external Crome layer while the x86 bridge remains an ABI/runtime adapter.


## Automatic Target loop runtime-confirmed

The external x64 target loop was validated live against multiple consecutive Rats:

- selected Rat by semantic name without Follow/manual Attack;
- tracked HP transitions (for example 100 -> 85 -> 65 -> 30 -> 15);
- when the current Rat disappeared/died, automatically selected a new Rat ID;
- repeated across multiple targets until the user stopped the loop with CTRL+C.

A transient pipe reconnect race was observed between one-request pipe instances. The x64 client now retries short ERROR_FILE_NOT_FOUND / ERROR_PIPE_BUSY windows locally instead of surfacing them as target failures.


## Bridge V7 game-thread target resolution

After a live UI test produced an access violation in the Ramonia process, the ATTACK_ID handoff was hardened. The pipe side no longer stores a resolved Monster pointer. It stores only creatureId; the game-thread dispatcher resolves the current Monster pointer and validates it immediately before the native attack call. This specifically reduces stale-pointer risk when a creature dies/despawns during retargeting.


## Bridge V8 targeting performance

Live testing confirmed FPS drops only while Target was active. The cause was the provisional full MEM_PRIVATE/vtable scan being executed multiple times per second.

V8 adds a 1000 ms monster snapshot cache. GET_VISIBLE_MONSTERS reuses the cache, and ATTACK_ID uses the cached address only after game-thread vtable + creature-ID revalidation. This materially reduces scan frequency without changing semantic target selection.


## Bridge V9 dispatcher isolation fix

A/B testing established that Ramonia stayed stable when no bridge was injected, but could crash at creature death with the bridge loaded even while Target was disabled. This isolated the permanent WndProc subclass as the leading bridge-level suspect rather than Target selection or ATTACK_ID itself.

V9 removes SetWindowLongPtr/GWLP_WNDPROC entirely. Game-thread execution now uses a thread-specific WH_GETMESSAGE hook + PostThreadMessage + completion event. Dispatches are tokenized to invalidate late messages after timeout. The V8 scan cache is retained.


## Crome v0.8.1 empty-target idle guard

Live testing found a distinct UI/worker bug: with Target enabled, Attack Any disabled, and an empty target-name list, the external worker still polled GET_VISIBLE_MONSTERS and GET_COMBAT_TARGETS even though no monster could ever match.

v0.8.1 treats this configuration as a true idle state. It performs no monster scan/combat-target polling until either at least one target name is configured or Attack Any is enabled. Empty target rules are also ignored during name matching.


## Bridge V10 Tile::things read-only proof

The next crash-isolation step does not modify Target behavior. V10 adds GET_TILE_MONSTERS as a one-shot read-only diagnostic.

It scans exact Tile-vtable objects, validates the runtime-confirmed Tile::things vector (+0x24/+0x28/+0x2C, elem 4), requires a stable vector header during the snapshot, and reports only same-floor nearby Monster objects that are actual members of a Tile::things collection.

No attack/follow/internal call is performed by this diagnostic. The existing GET_VISIBLE_MONSTERS heap scanner and ATTACK_ID path remain unchanged until this Tile-membership proof is validated live.


## Bridge V11 cached Tile Target resolver

V10 live proof succeeded with moving/damaged creatures and multiple same-name Spiders. V11 promotes Tile::things from diagnostic-only to the Target resolver.

GET_VISIBLE_MONSTERS now reads Monsters only as members of cached Tile::things. ATTACK_ID prevalidates the semantic ID against Tile membership on the pipe thread and then resolves the ID again from current Tile::things on the game thread before Game::attack.

The previous broad Monster heap scanner is no longer on the Target/ATTACK_ID execution path. Broad memory scanning remains only as an occasional Tile-address discovery step and is not executed on the game thread.


## Bridge V12 on-demand dispatcher isolation

A crash was reproduced in town with no active target/monster interaction, with the same 0x01037419 access-violation signature. Since the V11 Tile target path was idle at the time, the always-installed WH_GETMESSAGE hook became the principal remaining bridge-level suspect.

V12 removes the permanent hook. Startup only resolves the game window/thread. A WH_GETMESSAGE hook and completion event are created only for an ATTACK action and are removed immediately after the action (including timeout/error cleanup). Tile::things caching/Target semantics remain unchanged from V11.


## Bridge V13 corpse transition discovery

After V12 remained stable under extended cave combat, Loot discovery starts with a read-only Tile transition probe.

LOOT_MARK captures cached Tile::things Items plus nearby Monster membership. LOOT_DIFF reports new exact-Item objects and Monsters that disappeared, correlated by the same cached Tile address internally and exposed only through ephemeral tileSlot diagnostics.

No corpse is opened and no item is moved in V13. The goal is to identify the real corpse client item ID(s) produced by creature death before validating the Game::use/container-open ABI.


## Bridge V14 active loot ABI test

V13 correlated Rat/Cave Rat disappearance with new Item 2813 on the exact death Tiles (plus transient Item 2016). V14 promotes this only to an experimental one-shot use path; 2813 is not persisted as a universal corpse ID.

Game::use and Game::move use their V37 runtime-confirmed shared_object_ptr_ref contracts. The bridge passes addresses of the actual 4-byte Tile::things / Container::items holder elements rather than raw Item pointers. The V12 on-demand WH_GETMESSAGE lifetime remains unchanged.

GET_OPEN_CONTAINERS reads the exact Container RTTI graph rooted at the runtime-confirmed Game+0x10 inline registry. MOVE_CONTAINER_ITEM is prepared for gold 2148 -> an already-open Bag. Its special container Position encoding is explicitly experimental pending this live test.


## V15 loot snapshot correction

Live V14 evidence showed Container registry decoding is correct (container id/name/capacity/items and gold 2148 were read successfully). The active use call was not reached because the corpse candidate filter required the new Item to appear on the exact Tile occupied by the monster at LOOT_MARK. In the observed run the Rat moved before death, so corpse candidate 2813 appeared on a different Tile.

V15 stores the exact Tile cache address list at LOOT_MARK and rereads that frozen Tile set at LOOT_DIFF. It no longer rejects the diff merely because the global Target cache generation changed. When at least one tracked monster disappears, newly-added Items in the marked snapshot are retained as death-window candidates; USE_LOOT_ITEM still revalidates the requested Item ID and exact holder on the game thread before calling Game::use.


## V15 live move confirmation

Live validation completed successfully:
- source Container: id=1, name=`dead rat`
- destination Container: id=0, name=`bag`
- moved Item: gold id=2148, count=3
- before: Bag gold=27, dead rat gold=3
- after: Bag gold=30, dead rat no longer contained gold

This confirms the active `Game::move` path end-to-end for this build, including:
- Item `shared_object_ptr_ref` holder ABI;
- runtime-confirmed Position ABI;
- runtime-confirmed Count ABI;
- destination container encoding `x=0xFFFF, y=0x40|containerId, z=slot`.

The bridge now reports `destinationEncoding=otclient_container_position_runtime_confirmed` for this path.


## V16 container use / Auto Food foundation

The live V15 `Game::move` confirmation also validated the 4-byte `Container::items` holder representation. V16 reuses the already runtime-confirmed `Game::use(Item shared_object_ptr_ref)` ABI for items that are already inside open containers.

New bridge command:

`USE_CONTAINER_ITEM`

Arguments:
- `itemId` — required u16 item id;
- `sourceContainerId` — optional. If omitted, the bridge searches every currently open container and uses the first matching item.

The x64 pipe test exposes:
- `useitem <containerId|any> [itemId] [PID]`
- `usefood <containerId|any> [itemId=2696] [PID]`

For the first live validation, cheese id `2696` is the default food candidate. This path is intentionally independent from Loot: any matching food already present in an open bag/container may be used regardless of whether it came from a corpse, the player, or another container.

V16 keeps the existing safety rule: the selected Container, holder address, Item pointer, and item id are revalidated on the game thread immediately before `Game::use`. Continuous Auto Food scheduling is not enabled until this one-shot container-use path is confirmed live.


## V16 live container-use confirmation

Live validation succeeded with cheese id `2696` already inside the player's open Bag:

- `GET_OPEN_CONTAINERS` resolved Bag id=0 and cheese at slot=2;
- `USE_CONTAINER_ITEM` returned `result=success`;
- the live Ramonia client consumed the cheese;
- holder ABI remained `shared_object_ptr_ref`.

This confirms `Game::use` end-to-end for a Container::items holder, not only a Tile::things holder.

The x64 pipe client now also exposes a continuous policy loop:

`autofood [itemId=2696] [successDelayMs=60000] [retryDelayMs=2000] [PID]`

The loop lives outside the x86 bridge. It repeatedly asks the bridge to use the configured food in any open container. After a successful use it waits `successDelayMs`; while food is absent it polls using `retryDelayMs`. This preserves the architecture rule that the x64 Crome layer owns bot policy/scheduling while the x86 bridge remains a validated ABI adapter.


## V16 UI integration and multi-container live confirmation

Extended live Auto Food validation confirmed that `USE_CONTAINER_ITEM` searches across multiple open containers rather than only Bag id=0. Cheese id `2696` was consumed successfully from container ids 0, 1, 2 and 3 during one continuous `autofood` session. When no matching food was present, the loop waited; when a new cheese appeared later, it was detected and consumed without restarting the client.

The Crome UI now wires the Tools -> Food Eater control to the Ramonia V16 bridge. Food policy remains in the external x64 Crome layer, with configurable Food ID and interval, while the injected x86 bridge remains the validated ABI adapter. Other Tools remain disabled on Ramonia until their own backends are confirmed.


## V16 Crome UI live confirmation

Final live validation through the actual Crome UI succeeded. With Tools -> Food Eater enabled, Food ID 2696 and the Ramonia V16 bridge injected, Crome consumed food correctly without using CromePipeTest. This confirms the production path:

Crome x64 UI/policy -> named pipe -> Ramonia x86 bridge V16 -> validated Container::items holder -> Game::use.

Auto Food is therefore considered runtime-confirmed for this Ramonia build. CromePipeTest remains only as a diagnostic/manual validation tool for this feature.


## V17 one-cycle Auto Loot diagnostic

A one-cycle external Auto Loot diagnostic was added to CromePipeTest without changing the validated x86 bridge contracts.

Command:

`autoloot [corpseId=2813] [targetBagId=0] [lootItemId=2148] [waitMs=30000] [PID]`

The diagnostic performs this sequence:

1. reads currently open containers and verifies that the target bag is open;
2. calls `LOOT_MARK`;
3. polls `LOOT_DIFF` until a death-window corpse candidate with the requested corpse item id appears;
4. calls `USE_LOOT_ITEM` to open the corpse;
5. detects the newly opened corpse container;
6. calls `MOVE_CONTAINER_ITEM` to move the requested loot item into the target bag;
7. rereads the target bag and reports the resulting total.

Initial defaults are intentionally limited to the already observed Ramonia Rat/Cave Rat evidence:
- corpse item id: `2813`;
- target bag id: `0`;
- loot item id: gold `2148`.

This stage is diagnostic-only. Continuous Loot in the Crome UI remains disabled until the complete one-cycle chain is confirmed live.


## V17 live Auto Loot confirmation and Crome UI integration

The one-cycle Auto Loot diagnostic was live-confirmed twice against Rat corpses.

Confirmed sequence:
- corpse item id `2813` detected after Rat death;
- corpse opened through `USE_LOOT_ITEM`;
- newly opened container identified as `dead rat`;
- gold item id `2148` moved through `MOVE_CONTAINER_ITEM`;
- destination bag id `0` updated successfully.

Observed successful move counts included 3 gold and 1 gold in separate runs. The diagnostic also reported the destination Bag total after each move.

Two earlier diagnostic runs timed out before a corpse candidate was detected. To remove that manual timing weakness from normal use, Crome v0.8.3 arms `LOOT_MARK` only when an actual AttackTarget is present. It keeps one baseline per target, waits for that target ID to disappear, then runs `LOOT_DIFF`, opens the configured corpse and moves the configured loot items. After a Crome `ATTACK_ID`, Loot also arms its baseline immediately in the same worker cycle so very weak targets cannot die before the next 300 ms poll.

Ramonia Loot defaults:
- corpse item id: `2813`;
- destination container id: `0`;
- loot-only-list: enabled;
- initial loot item list: `2148` (gold).

The Ramonia Loot policy is external x64 Crome logic. The x86 bridge remains unchanged at V16 and continues to expose the already runtime-confirmed semantic actions.


## Ramonia transition crash — safety hardening pending live validation

A crash was reported after leaving the cave with Target/Loot active, after successful cave combat. The existing V12+ dispatcher contract was audited and still removes its `WH_GETMESSAGE` hook immediately after each native action, including timeout/error cleanup; no permanent Target hook is installed while idle.

Two additional protections were added before the next live run:

- Crome v0.8.4 detects Ramonia floor changes or large position jumps through `GET_PLAYER`, clears pending Loot target state and pauses all `Tile::things` Target/Loot polling for 1600 ms while the map settles.
- Bridge V17 re-resolves the current Ramonia window and game-thread id immediately before every native action. It no longer reuses a cached thread id across actions/map transitions. The per-action hook is still installed only for the action and removed immediately afterward.

These changes are safety hardening based on the observed transition crash. They are not considered runtime-confirmed until a live cave-exit test completes without a crash.

Target name filtering remains policy-driven: a list containing only `Rat` intentionally excludes `Cave Rat` and `Spider`. Those names must be added explicitly or Attack Any (`*`) must be enabled.

Loot corpse id `2813` is confirmed only for the tested Rat corpse. Other creature types should have their death-window corpse item id captured before being added to continuous Loot.


## Pre-V17 A/B isolation: transition crash points to Target/Tile cache

Live A/B testing on the previous version narrowed the crash significantly.

Stable case:
- Auto Food enabled;
- Loot enabled;
- Target disabled;
- Rat and Cave Rat corpses opened and looted correctly;
- no crash, including leaving the cave.

Crash case:
- Target enabled for Rat and Cave Rat;
- Loot enabled;
- Auto Food enabled;
- all configured monsters were killed and looted correctly inside the cave;
- shortly after climbing the stairs and changing floor/route, the client crashed.

This indicates the validated container use/move path is not the primary trigger. The failure correlates with Target/Tile::things activity across a floor transition.

Additional V17 hardening:
- Crome v0.8.4 pauses Target/Loot for 1600 ms after a z change or large position jump;
- the bridge re-resolves the game window/thread before every on-demand action;
- the bridge now invalidates the cached Tile* list immediately when LocalPlayer z differs from the cache origin z, rather than allowing the previous-floor cache to survive for up to 1000 ms.

Rat and Cave Rat both opened successfully under corpse item id `2813` on this Ramonia build.


## V18 Target isolation diagnostics

Further live testing showed the crash can also occur inside the cave when Target is active, while Loot + Auto Food without Target remain stable. This broadens the fault from floor-transition-only to the Target path itself.

V18 introduces two isolation aids:

- `CromePipeTest_x64.exe targetscan <name|*>`: continuous read-only Target polling using `GET_VISIBLE_MONSTERS` + `GET_COMBAT_TARGETS`; it never calls `ATTACK_ID` and never installs the game-thread hook.
- bridge Target trace markers in `CromeRamoniaBridge.log`: ATTACK_ID request, hook installation, before/after `Game::attack`, and hook removal.

The validated V37 attack ABI remains:
`x86 thiscall-like, ECX=Game*, arg1=Creature* raw_ptr, arg2=bool false`.

The next A/B test is intentionally split:
1. read-only targetscan while manually killing/moving/changing floors;
2. active target loop only if read-only remains stable.


## V19 Target ownership correction — probable root cause

Live testing showed that Target can trigger delayed crashes even inside the cave, while Loot + Auto Food remain stable without Target.

Cross-checking the Ramonia attack ABI against OTC-family source exposed a classifier error in the previous V34/V37 interpretation:

- OTC `Game::attack` takes `CreaturePtr creature` by value, not a non-owning raw `Creature*`;
- this OTC family uses pointer-sized `stdext::shared_object_ptr<Creature>`;
- therefore the x86 stack slot contains the same pointer bits as a raw Creature*, but the caller must perform the smart-pointer copy ownership step (`add_ref`) before entering `Game::attack`;
- the callee releases the by-value parameter on return.

The previous ABI Builder special-cased attack/follow Creature arg0 as `raw_ptr` before smart-pointer ownership semantics were considered. That allowed ATTACK_ID to work immediately while potentially under-counting the Creature reference and leaving Game::m_attackingCreature with invalid ownership after a creature disappeared.

V19 changes:
- attack/follow arg0 is now `shared_object_ptr_value`;
- profile records `refCountOffset=0x4` and `ownership=caller_add_ref_callee_release`;
- bridge validates the intrusive refcount and increments it before the native Game::attack call, matching the normal C++ caller's CreaturePtr copy step;
- Crome Target requires Bridge V19+; Food/Loot remain V16+;
- ABI Builder persists this ownership metadata for future profile regeneration.

This is a root-cause correction, but remains pending live confirmation. Do not mark Target stable until V19 survives repeated attacks, creature deaths, walking, and floor transitions.


## V19 stability signal + sticky Target policy

First live V19 run remained stable for at least 10 minutes under active targeting, which is the strongest evidence so far that the CreaturePtr ownership correction addressed the delayed crash mechanism.

A separate Target-policy issue was observed: a Troll could be left alive when its health became low/red because the external selector immediately chose another monster whenever the current AttackTarget was absent from a single Tile::things poll or no longer matched acquisition filters.

Crome v0.8.6 and CromePipeTest now use sticky AttackTarget semantics:
- while Game::AttackTarget is nonzero, no new target is acquired;
- if the active target appears in the monster snapshot, it is retained regardless of HP/range acquisition filters;
- if one Tile::things poll misses the active target, the worker waits rather than switching;
- a new target is selected only after the game itself clears AttackTarget.

This separates target acquisition policy from active-target retention and should prevent low-HP monsters from being abandoned before death.


## V19 extended stability + V0.8.7 Auto Corpse

A subsequent live run with the V19 CreaturePtr ownership correction remained stable for at least 15 minutes under active Target use, including repeated retargeting. No delayed crash was observed during that interval.

Loot reliability was then reported at roughly two-thirds of kills while multiple creature species were being targeted. The previous Crome logic still required one configured corpse item id (2813) and attempted corpse opening with a single fixed timing assumption.

Crome v0.8.7 changes Loot to death-window Auto Corpse:
- LOOT_DIFF now supplies the item ids created when the tracked AttackTarget disappears;
- candidates born on the exact target tile are preferred;
- configured corpse id 2813 remains only as a fallback/proven candidate for Rat/Cave Rat;
- other death-window item ids can be tried automatically, allowing Spider/Troll/edited creature corpse ids to be discovered without hardcoding each species first;
- corpse opening now waits up to about 1.2 seconds for a new or reused container window rather than assuming 350 ms is always sufficient;
- reused container ids are detected by snapshot change and do not depend on the container name starting with "dead";
- the death remains pending for up to 4 seconds and is cleared only after a corpse opens successfully or the deadline expires.

Bridge remains V19; this reliability change is external Crome policy only.


## V20 / Crome v0.8.8 Death-Tile Loot

Live testing of v0.8.7 exposed an over-broad Auto Corpse fallback. Decorative corpse-like map objects (for example a dead bear or decorative player body) could enter the generic death-window candidate list and stall the external worker while Crome repeatedly tried to open them. Target itself remained stable under V19.

The Loot model is now target/tile-bound:

- Bridge V20 adds `LOOT_MARK_TARGET <creatureId>`.
- The marker commits a new Loot baseline only when that exact AttackTarget is currently visible in `Tile::things`.
- Crome refreshes this target-specific baseline roughly every 450 ms while the target is alive, preserving the last good baseline if the target disappears between polls.
- The baseline stores the target ID and its last confirmed Tile address.
- On `LOOT_DIFF`, candidates are restricted to Items that are newly present on that exact tracked Tile after the tracked target disappears.
- Existing decorative bodies/items on the Tile were already present in the baseline, so they are not candidates.
- Items added on other Tiles are never exposed to normal Crome Loot.
- Crome removed the v0.8.7 `allAdded` fallback completely.
- Corpse ID 2813 remains only a preferred fallback when it is itself one of the strict death-tile candidates.
- Loot now requires Bridge V20+; Target still requires V19+; Food still requires V16+.

This implements the intended rule: only the body/object created on the Tile where the tracked target actually died is eligible for corpse opening. Decorative map corpses must remain ignored.


## V21 / Crome v0.8.9 Rolling Loot

After V20/v0.8.8 removed decorative corpse false positives, Loot still succeeded on only about one third of kills. The remaining failure mode is consistent with last-moment creature movement and death-transition timing.

V21 changes the target-specific Loot baseline from one Tile to a rolling history of the last six confirmed Tiles occupied by the same live AttackTarget. Each frame stores the item identities already present on that Tile while the target was alive.

On death:
- Loot checks only those recent target Tiles;
- an item is a corpse candidate only if it is newly present relative to that Tile's own live-target baseline;
- decorative corpses/items already present on those Tiles remain excluded;
- duplicate candidates are suppressed;
- the baseline is never updated from a target snapshot with hpPercent=0, preventing a just-created corpse from being accidentally absorbed into the baseline.

Crome v0.8.9 refreshes the target-specific Loot mark every worker cycle (~250-300 ms) and requires Bridge V21+ for Ramonia Loot. Target remains V19+ and Food remains V16+.


## V21 stability >1h + Crome v0.9.0 Learned Loot

Live testing reported more than one hour without a Ramonia crash under active Target/Loot/Food. This is strong runtime evidence that the V19 CreaturePtr ownership correction and later V21 Loot tracking do not reintroduce the previous delayed Target crash.

A remaining Loot reliability issue appears mainly in crowded fights. Review of the external worker confirmed that Target is not racing ahead of Loot: while a death is pending, the worker holds target selection and continues the Loot state machine first.

The crowded-fight bottleneck is corpse identification cost. A strict death tile can contain several newly-created Items (blood/splash/field/etc.). Previously every unknown candidate could receive the full ~1.2 s asynchronous container-open wait, exhausting the death window before the real corpse candidate was reached.

Crome v0.9.0 adds runtime corpse learning:
- corpse IDs that successfully open a container are remembered for the current Crome session;
- known corpse IDs are prioritized before unknown strict death-tile candidates;
- known IDs receive the full ~1.25 s asynchronous-open budget;
- unknown candidates receive a short ~320 ms probe;
- at most six unknown candidates are probed per death;
- the overall death window is extended to ~5.5 s;
- strict V21 rolling death-tile filtering remains unchanged, so decorative bodies outside/newly-unrelated to the tracked death do not return as candidates.

Bridge remains V21 for this step; v0.9.0 is an external Crome policy optimization.


## V22 / Crome v0.9.1 Persist + Light

Loot corpse learning is now persistent. Crome stores automatically confirmed corpse IDs in:
`loot.json -> learned_corpse_ids`.

The manually editable `corpse_item_ids` value remains a fallback and is not overwritten by automatic learning. On startup, Crome reloads `learned_corpse_ids`; when an unknown strict death-tile candidate successfully opens a corpse container, the ID is added to the shared Loot config and persisted by the UI JSON writer.

Ramonia Light is now wired from the existing build-profile contract rather than from a copied/hardcoded Classic offset.

Current profile evidence for build `6AB03F94 / E281E9CA5BA0AF2`:
- `UIMap` vtable RVA: `0x774284`
- `MapView` vtable RVA: `0x773E60`
- `UIMap -> MapView`: `+0x2EC`
- `MapView::drawLights`: `+0x1D4` (`u8`)
- `MapView::minimumAmbient`: `+0x1D8` (`float`)
- contract source: matching setter/getter wrapper shapes for draw-lights and minimum-ambient in this exact build profile.

Bridge V22:
- discovers UIMap instances by the exact profile vtable;
- validates the linked MapView by its exact profile vtable;
- snapshots the original `minimumAmbient`;
- `SET_LIGHT enabled=1` writes ambient `1.0f`;
- `SET_LIGHT enabled=0` restores the original value;
- no Target/game-thread hook is installed for Light;
- exposes `GET_LIGHT`, `SET_LIGHT`, and HELLO `lightReady/lightEnabled`;
- attempts restore on bridge-thread shutdown.

Crome v0.9.1:
- enables the existing Tools -> Light Hack checkbox for Ramonia only with Bridge V22+;
- refreshes Light every ~500 ms so floor/map changes can reapply the desired ambient;
- keeps all other unsupported Ramonia Tools gated;
- adds `lightstatus`, `lighton`, and `lightoff` diagnostics to CromePipeTest.

This V22 Light backend is profile-driven but has not yet been runtime-confirmed visually on Ramonia. Compilation/runtime validation remains pending the next live test.


## V22 Light runtime-confirmed + Crome v0.9.2 Fast Reacquire

Ramonia Light V22 was visually confirmed live:
- SET_LIGHT/lighton changed the same dark cave area to full-bright;
- the profile-driven UIMap -> MapView -> minimumAmbient path is therefore runtime-confirmed for build 6AB03F94 / E281E9CA5BA0AF2;
- Light remained compatible with active Target/Loot during the test.

A separate responsiveness issue was then observed: after a monster died, another nearby monster could attack the player for roughly 5 seconds before Crome selected it.

Code review identified the exact cause: the Loot state machine can keep a previous death pending for up to ~5.5 seconds, and its wait path used `continue` before Target acquisition.

Crome v0.9.2 changes scheduling only:
- if Game::AttackTarget is zero, Target acquisition runs before pending Loot processing;
- a new monster can therefore receive ATTACK_ID immediately while the previous corpse is still being resolved;
- the existing pending Loot baseline is deliberately not overwritten by the fast reacquire path;
- the normal Loot marker arms the new AttackTarget after the previous death is resolved;
- new target acquisition ignores hpPercent=0 creatures.

Bridge remains V22; no Game::attack ABI, dispatcher, Light backend, or Loot bridge contract changed in this step.


## Bridge V23 / Crome v0.9.3 Healing + Nested Loot

### Internal spell / SAY
Ramonia profile already had Game::talkChannel runtime-confirmed:
- method RVA: 0x79750
- x86 thiscall-like
- arg0 MessageMode u32
- arg1 Channel u32
- arg2 MSVC std::string const-ref

For the normal SAY path the build profile now stores:
- messageSayValue: 1
- defaultChannel: 0
- source/status: OTC-family Game::talk -> talkChannel(MessageSay, 0, message), pending Ramonia live spell confirmation.

Bridge V23 adds an on-demand PendingTalk action through the same temporary game-thread dispatcher used by the stable native actions. Because the bridge is MinGW while Ramonia uses MSVC ABI, it does not pass a MinGW std::string. It builds the 24-byte x86 MSVC small-string layout manually and intentionally limits SAY messages to 15 ASCII characters. This covers exura, exura gran and exura vita.

Pipe diagnostic:
- CromePipeTest_x64.exe say "exura"

Crome Healing:
- spell Healing is available on Ramonia with Bridge V23+;
- reads GET_PLAYER HP/maxHP/mana;
- chooses highest-priority matching spell rule: High -> Mid -> Light;
- default cooldown remains 850 ms;
- UH/Mana Fluid/Life Fluid remain native-only until their self-use ABI is validated.

### Recursive nested corpse containers
Some monsters can place loot inside a bag/container inside the corpse. Crome v0.9.3 adds safe recursive Loot:
- loot.json field: nested_container_ids
- UI field: Nested bag IDs
- existing global Walker IDs -> Corpse Bag IDs is used as fallback when nested_container_ids is empty
- only explicitly configured container item IDs are opened; arbitrary unknown corpse items are never auto-used
- direct configured loot is moved first, then configured nested bags are opened and their contents processed
- nested depth is capped at 3
- nested containers are opened through the already-confirmed USE_CONTAINER_ITEM sourceContainerId path
- normal corpse learning / rolling death-tile logic remains unchanged.

The nested-bag feature requires the actual bag item ID for the server/build and therefore remains calibration-dependent until a live nested corpse is observed.


## Bridge V24 / Crome v0.9.4 Loot UI + Self-Use Prep

UI cleanup:
- Loot/Looter uses balanced columns so Destination Bag ID is fully visible.
- Nested bag configuration is labeled Corpse Bag IDs and remains backed by nested_container_ids.
- Loot IDs and Corpse Bag IDs are shown side-by-side.
- Ground Drop is visually separated from the Ramonia corpse workflow.
- Tools card height was reduced without changing backend behavior.

UH / potion preparation:
The Ramonia build profile already has Game::useWith runtime-confirmed at RVA 0x784F0 with:
- arg0 Item = shared_object_ptr_ref
- arg1 Thing = shared_object_ptr_ref

Game::localPlayer is stored at Game +0x0 and the LocalPlayer vtable is available in RTTI. Bridge V24 adds a diagnostic USE_CONTAINER_ITEM_ON_SELF path:
- finds the requested item in an open container;
- revalidates the real Container::items holder on the game thread;
- validates the current LocalPlayer object;
- calls Game::useWith(const ItemPtr&, const ThingPtr&) with the real item holder and a pointer-sized ThingPtr view of the current LocalPlayer;
- does not yet enable automatic UH/Mana Fluid/Life Fluid rules.

Pipe diagnostic:
CromePipeTest_x64.exe useself <containerId|any> <itemId> [PID]

Automatic item Healing must remain disabled until one live self-use is confirmed. After confirmation, the existing Healing HP/Mana rule engine can route UH/Life Fluid/Mana Fluid through this backend.


## Crome v0.9.5 Look IDs

The user preferred an item-ID inspector over exposing more manual ID fields in Healing.

Crome now adds a Ramonia Look / IDs popup driven by GET_OPEN_CONTAINERS:
- open a bag or corpse in the game;
- press Look IDs from Healing or Loot;
- the popup lists Container ID, container name, slot, Item ID and count;
- each row has a one-click ID copy button;
- Refresh rereads the currently open containers.

This intentionally avoids a permanent Game::look hook. It is meant for calibration of UH, potions, nested corpse bags and other container items while preserving the stable on-demand bridge design.


## Bridge V25 / Crome v0.9.6 Real Look ID

The first v0.9.5 Look IDs implementation only listed item IDs from currently open containers. Live feedback showed this was not the intended workflow.

V25 now implements a one-shot capture of the player's normal Game::look action:
- Crome arms Look ID for up to 8 seconds;
- Bridge installs a temporary INT3 only on the runtime-confirmed Game::look entry;
- a temporary VEH captures the shared_object_ptr_ref Item argument from the real look call;
- the Item object is validated and itemId is read from the profile-confirmed Item layout;
- the original first byte is restored immediately on the first Game::look;
- EIP is rewound so the game's original Game::look executes normally;
- the breakpoint and VEH are also removed automatically on timeout;
- shutdown forcibly restores/cleans any pending capture.

No Game::look hook/breakpoint remains active while idle.

Bridge commands:
- ARM_LOOK_ID
- GET_LOOK_ID

Pipe diagnostics:
- CromePipeTest_x64.exe lookarm
- perform normal Look on one item in Ramonia
- CromePipeTest_x64.exe lookget

Crome v0.9.6 changes the Healing/Loot Look ID button to arm this real one-shot capture and displays the captured Item ID with a copy button.


## Crome v0.9.7 Passive Look ID

UI correction after live feedback:
- removed Look ID buttons from Healing;
- removed Look ID buttons from Loot;
- removed the Look ID popup entirely;
- the only user-facing Look result is the existing Runtime / Build footer line: `Look ID: ...`.

With Ramonia Bridge V25+ ready, Crome now passively maintains the one-shot Game::look capture:
- polls GET_LOOK_ID roughly every 150 ms;
- when no capture is armed/busy, automatically sends ARM_LOOK_ID;
- after the user performs a normal Look in Ramonia, the captured item ID is copied into the existing `g_lastLook` display state;
- once the one-shot completes, Crome automatically re-arms after a short delay;
- the legacy look_result.json path is not allowed to overwrite the Ramonia V25 captured ID.

There is no manual Look button in the module cards anymore. Normal workflow is simply: perform Look in Ramonia -> Runtime / Build shows the last captured item ID.


## Crome v0.9.8 Editable Item Healing

Ramonia item-on-self Healing is now exposed in the normal Healing UI using the Bridge V24+ backend (Bridge V25 includes it).

UI:
- UH row: HP trigger %, editable Item ID, enable checkbox.
- Mana Fluid row: MP trigger %, editable Item ID, enable checkbox.
- Life Fluid row: HP trigger %, editable Item ID, enable checkbox.
- IDs stay editable regardless of backend readiness; enable checkboxes require a compatible backend.
- current OT Mana Fluid default is 15552, but this is configuration only and remains editable for other OTs.

Runtime:
- item rules use USE_CONTAINER_ITEM_ON_SELF and scan open containers for the configured item ID;
- UH priority 1000;
- Life Fluid priority 900;
- spell rules remain 800/700/600;
- Mana Fluid priority 500;
- if a higher-priority item heal is configured but unavailable in open containers, Crome can fall back to a matching spell rule instead of leaving the player unhealed;
- item cooldown default is 650 ms.

Persistence:
- item IDs and enable state continue to be stored in healing.json;
- an old healing.json containing mana_fluid item_id=0 no longer overwrites the current OT default 15552 on first load.


## Bridge V26 / Crome v0.9.9 Rune Trainer + Friends Prep

### Rune Maker / Mana Trainer
Ramonia GET_PLAYER now exposes Soul as an optional player field:
- selectedFields.soul is read when present/valid;
- response includes soulAvailable=true/false;
- soul is emitted only when available;
- Soul is not a required profile field, so builds/OTs without Soul can still use the Trainer.

Crome v0.9.9 enables Rune Maker / Trainer on Ramonia Bridge V26+:
- Enabled;
- editable Spell;
- Mana threshold in percent (cast when current Mana% >= configured threshold);
- Soul Min;
- Soul mode is automatic: if the build exposes Soul, require current Soul >= Soul Min; if Soul is unavailable, ignore Soul entirely;
- default cooldown 1000 ms;
- spell is sent through the already-confirmed internal SAY/talkChannel backend;
- Healing has priority over Rune Trainer in the same worker cycle.

Rune configuration persists in rune.json.

### Friends / Party preparation
Friends / Party remains preparation-only pending a suitable live OT test. friends.json persists:
- Friend HP;
- Your HP;
- Follow Friends;
- UH Friends;
- Party Mode;
- friend name CSV list.

Prepared future semantics:
- druid heal: exura sio "{name}";
- UH friend: useWith UH on the selected friend ThingPtr;
- follow: follow friend by configured name;
- Party Mode: green shield / green skull ally semantics pending runtime validation.

No Friend/Party native action is enabled yet.


## Crome v0.9.10 Compact UI

UI-only cleanup before Cavebot work:
- Tools card height is reduced on Ramonia; native backend keeps extra height when needed.
- Loot / Looter card height is reduced substantially on Ramonia, removing the large blank area below Ground Drop.
- Rune Maker / Trainer card height is reduced on Ramonia while preserving the larger legacy/native layout.
- No Target, Loot, Healing, Light, Food, Trainer, Bridge, or Cavebot behavior changed.


## Crome v0.9.11 Cavebot UI Fix

The Cavebot card was being clipped by the fixed-height left column. After Healing + Friends + Rune, only about 208 px remained, while Cavebot still requested 245 px.

UI fix:
- Cavebot now consumes exactly the remaining height of the left column;
- the waypoint list viewport is reduced but remains independently scrollable;
- the Profile caption is shortened to avoid wrapping;
- bottom Cavebot controls (Keep Follow, Attack Any, Range, Should Attack Me, Mode) remain visible;
- Runtime / Build footer remains visible;
- no Cavebot/Target/Loot/Healing backend behavior changed.


## Crome v0.9.12 Cavebot Compact

Second Cavebot UI pass after live screenshot:
- Cavebot child now uses exactly the remaining parent height and never requests more than the fixed left column can display;
- waypoint viewport is reduced;
- Add/Edit/Remove/Record controls are arranged as a compact 2x2 block instead of four vertical rows;
- the extra empty-target hint row was removed;
- bottom Target controls remain visible without covering Runtime / Build.

No Cavebot/Target backend behavior changed.


## Bridge V28 / Ramonia Cavebot backend

Cavebot backend was completed without further UI/layout changes.

### Movement
Bridge V27 introduced AUTO_WALK_TO using the build-profile runtime-confirmed Game::autoWalk contract:
- RVA 0x76860;
- x86 thiscall-like, ECX=Game;
- Directions argument = vector_direction_u32_ref;
- bridge builds the 12-byte MSVC x86 vector view manually;
- directions use the OTClient enum North=0, East=1, South=2, West=3, NE=4, SE=5, SW=6, NW=7;
- route length is capped at 127 directions;
- action is dispatched on demand on the game thread.

Pipe diagnostic:
CromePipeTest_x64.exe walkto <x> <y> <z> [PID]

Crome Cavebot now executes the existing waypoint types:
- Walk / Stand: AUTO_WALK_TO, exact-position completion;
- Wait: external timer;
- Say: existing internal SAY/talkChannel;
- Use / Ladder: USE_TILE_ITEM_AT;
- Rope / Shovel / Hole(use_with_ground): USE_WITH_TILE_AT.

The Cavebot state machine:
- loops the existing waypoint list;
- advances only after the current action is confirmed;
- tracks progress and retries after ~3.5 s without movement;
- pauses while Target/combat is active;
- naturally pauses while Loot holds the worker on a pending corpse;
- participates in the existing floor/map transition guard.

Add/Record now use the shared Ramonia GET_PLAYER position snapshot when the native Olders runtime is not active.

### Tile actions
Bridge V28 adds Tile item positioning and tile-targeted actions:
- TileItemDiagnostic now reads Thing position x/y/z from the profile-confirmed Thing base layout;
- findTileItemAt(x,y,z,id?) resolves the real Item in Tile::things;
- USE_TILE_ITEM_AT revalidates the actual Tile::things holder before Game::use;
- USE_WITH_TILE_AT finds the tool item in an open container, revalidates both source and target holders, then calls runtime-confirmed Game::useWith;
- no mouse simulation is used.

For Rope/Shovel/Hole, the configured Item ID is the tool item ID and the tool must currently be inside an open container. The target is the top matching Item at the waypoint tile.

Pipe diagnostics:
- CromePipeTest_x64.exe usetile <x> <y> <z> <itemId|0> [PID]
- CromePipeTest_x64.exe usewithtile <toolItemId> <x> <y> <z> [PID]

Bridge HELLO is now version 28 and continues to report walkReady. These Cavebot changes are not yet compile/live-confirmed; validate AUTO_WALK_TO first before running a full route.


## Bridge V29 / Research Probe V38 - Cavebot AutoWalk crash guard

First live Cavebot Enable test on Ramonia exposed a crash on the first AUTO_WALK_TO dispatch. The V28 status already marked AUTO_WALK_TO as pending live validation; therefore the crash is treated as an ABI-validation failure, not as a confirmed Cavebot route-state-machine defect.

### Root-cause finding
The V37 runtime ABI capture had an incomplete autoWalk model:
- `V33ExpectedArgs()` defaulted autoWalk to one argument;
- V37 therefore inspected and classified only `Directions`;
- `Directions = vector_direction_u32_ref` was genuinely observed;
- however, additional stack slots after Directions were never analyzed;
- consequently `actions.autoWalk.status=runtime_confirmed` could be promoted from an incomplete contract.

The strongest current hypothesis is an OTC-family autoWalk variant with a second logical start Position argument. This remains a hypothesis until V38 captures a natural game auto-walk and validates the stack evidence.

### Bridge V29 safety behavior
Bridge V29 now fail-closes AUTO_WALK_TO:
- the old V37 one-argument profile is no longer sufficient;
- the profile must contain a second logical `Position` argument with ABI `position_u32_u32_u8_value` and `runtime_confirmed`;
- even after profile confirmation, an explicit active-call implementation latch remains OFF until the V38 live result is reviewed and the call typedef/site is updated;
- HELLO reports `walkReady=false` while the latch is closed;
- AUTO_WALK_TO returns a safe contract/lock error instead of calling Game::autoWalk.

Crome also requires Bridge V29+ AND HELLO `walkReady=true` before enabling the Ramonia Cavebot backend. A saved `enabled=true` route therefore cannot accidentally re-enter the unsafe call.

### Research Probe V38
V38 deliberately recaptures autoWalk even when the imported V37 profile says it is runtime-confirmed.
It inspects:
- arg0 as the existing Directions vector reference;
- the next three x86 stack DWORDs as a candidate by-value Position layout: u32 x, u32 y, u8 z + padding;
- candidate xyz is checked against the live LocalPlayer position before the second logical argument can become runtime-confirmed.

V38 does not call autoWalk itself. The user must trigger a normal client auto-walk while the one-shot capture is armed.

### Next validation
1. Build `research/ramonia_x86/build_x86.bat`.
2. Confirm `RamoniaResearchProbe_V38.dll` and the V29 bridge are generated.
3. Use the Research Probe only for the natural autoWalk capture.
4. Review the V38 `[V38 AUTOWALK STARTPOS]`, raw stack, and resulting `actions.autoWalk.args`.
5. Only after that evidence is confirmed, implement the matching active Game::autoWalk call and reopen `walkReady`.

Do not test Cavebot Enabled as a movement test while `walkReady=false`.


## Bridge V30 - V38-confirmed AutoWalk ABI

Live V38 capture closed the Cavebot crash investigation.

Observed natural Game::autoWalk entry:
- ECX matched the exact Game owner;
- arg1 was a 12-element direction vector and classified as `vector_direction_u32_ref`;
- the next x86 stack DWORDs were `1266, 1284, 7`;
- V38 compared them with LocalPlayer `1264,1284,7` and confirmed the logical second argument as `Position`;
- final ABI: `Directions` by reference + `Position` by value, status `runtime_confirmed`.

The V28 crash was caused by the bridge calling the correct method RVA with an incomplete one-argument typedef.

Bridge V30 implementation:
- `GameAutoWalkFn` now receives `(Game*, DirectionsRef, MovePosition value)`;
- `MovePosition` is the existing 12-byte u32/u32/u8+padding layout;
- the real LocalPlayer position is copied into `startPos` before the route-direction builder mutates its working x/y values;
- the V38 safety latch is now enabled;
- HELLO reports bridgeVersion 30 and `walkReady=true` only when the V38 two-argument profile contract is loaded;
- canonical profile is promoted to schema/profileVersion V38 with both autoWalk arguments persisted.

Validation order:
1. build x86 package;
2. inject Bridge V30;
3. confirm HELLO `walkReady=true`;
4. run `CromePipeTest_x64.exe walkto <near_x> <near_y> <z> [PID]`;
5. only after direct walk succeeds, enable the Cavebot route.


### Live validation - AUTO_WALK_TO

Confirmed live on Ramonia after Bridge V30 / V38 profile update:
- direct `CromePipeTest_x64.exe walkto ...` moves the character successfully;
- no client crash observed on the corrected two-argument ABI;
- V38-confirmed call contract is therefore promoted from capture-confirmed to live-action-confirmed for this exact build;
- next validation stage is the Cavebot route state machine itself, beginning with Walk-only waypoints before tile actions.


## Cavebot Ramonia - repeated AutoWalk re-entry fix

A later live Cavebot test showed that direct AUTO_WALK_TO was stable, but repeated Cavebot use could still crash the client.

Root cause found in the external Cavebot state machine:
- after one AUTO_WALK_TO returned success, Cavebot scheduled another call about 750 ms later even if the character was still progressing toward the same waypoint;
- this could re-enter the native Game::autoWalk several times for one logical waypoint;
- the Ramonia worker also counted retries but did not enforce the configured max_retries value.

Fix:
- one native AUTO_WALK_TO is now issued per waypoint/approach;
- while the player position keeps changing, Cavebot only polls GET_PLAYER and does not call Game::autoWalk again;
- a new auto-walk is allowed only after no positional progress for stuck_ms (3500 ms);
- retry delay uses retry_delay_ms (650 ms);
- max_retries=2 is now enforced; after the initial attempt plus two retries the Cavebot halts that waypoint safely instead of repeatedly re-entering the native action;
- the same in-flight guard is used for Walk/Stand and for approach movement before Use/Rope/Shovel/Ladder/Hole;
- Ramonia Cavebot enable state now follows the central CaveRuntimeReady/walkReady gate instead of the older Bridge >=27 check.

This change is in the Crome Developer executable; Bridge V30 / V38 ABI remains unchanged.


## Bridge V31 - V39 formal AutoWalk ABI correction

A second live Cavebot crash occurred after the V30 two-argument call was enabled.

Crash report evidence:
- exception 0xC0000005 (access violation);
- crash addresses were in the Ramonia executable at low RVAs relative to the captured module base, not at Game::autoWalk RVA 0x76860;
- this is consistent with post-call stack/control-flow corruption rather than an immediate failure inside the auto-walk path builder.

ABI correction:
- V32 static function-body evidence had only proven a formal stack argument at [EBP+8] for Game::autoWalk;
- V38 observed three trailing DWORDs after Directions that matched LocalPlayer xyz, but stack adjacency alone did not prove those DWORDs were callee parameters;
- V39 therefore reclassifies those xyz values as caller-local/trailing stack evidence and removes Position from the formal action ABI;
- canonical autoWalk formal ABI is now Game::autoWalk(Directions&) with Game in ECX.

Bridge V31:
- GameAutoWalkFn is back to one formal Directions argument;
- the V30 Position-by-value argument is removed;
- bridge-level 500 ms native autoWalk cooldown added;
- action result code 39 = autowalk_cooldown;
- HELLO bridgeVersion = 31;
- walkReady is based on the corrected V39 one-argument profile contract.

Crome:
- Ramonia Cavebot requires Bridge V31+ and walkReady=true;
- old V30 bridge is rejected by the Cavebot gate;
- the one-request-per-waypoint / stuck-only retry controller remains in place.

Validation order:
1. rebuild Developer and x86 package;
2. inject Bridge V31 only after fully restarting Ramonia;
3. confirm HELLO bridgeVersion=31 and walkReady=true;
4. perform several alternating direct walkto calls manually;
5. only if repeated direct calls remain stable, enable a Walk-only Cavebot loop.


### Live validation - Bridge V31 repeated walkto

Confirmed live after the V39 formal ABI correction:
- repeated direct `CromePipeTest_x64.exe walkto ...` calls remained stable for about 5 minutes;
- no Ramonia crash observed during repeated alternating walks;
- Bridge V31 / one-argument Game::autoWalk(Directions&) is therefore promoted to repeated live-action validated for this exact build;
- next validation stage is the external Cavebot state machine using Walk-only waypoints in loop.


## V40 AutoWalk deep ABI investigation

Follow-up live testing clarified that Bridge V31 direct walkto is crash-stable but not reliably accepted by the game:
- many AUTO_WALK_TO requests returned `ok:true/result:success`;
- only a small subset produced actual player movement;
- therefore Bridge `success` currently means the native void method returned without immediate failure, not that Ramonia accepted/executed the route.

This reopens the formal ABI question.

Important evidence reconciliation:
- V32 static ABI scan inspected only the first 192 bytes of Game::autoWalk;
- that scan reported RET not seen and only EBP+8, but the function contains branches beyond +0x300, so the original scan window was insufficient to prove a one-argument ABI;
- V38 natural runtime capture observed Directions followed immediately by three DWORDs matching a plausible start Position x/y/z;
- V30 passed those values but used the generic compiler thiscall typedef and later crashed;
- V31 removed the Position and became crash-stable but frequently produces a no-movement `success`.

V40 is diagnostic-only and does not change the active Bridge V31 call:
- bounded deep static scan of autoWalk up to 0x1000 bytes / next semantic method;
- counts positive EBP/ESP frame references including +8/+C/+10/+14 candidates;
- reports plausible RET / RET imm16 epilogues and cleanup sizes;
- forces one natural autoWalk recapture even when the profile is already runtime-confirmed;
- prints 64 bytes before the natural callsite return address and 24 bytes after;
- resolves the direct CALL target when possible;
- identifies immediate caller cleanup patterns such as `add esp, imm`;
- prints trailing stack DWORDs through ESP+0x14.

Goal: determine whether the real contract is Directions-only, Directions+Position with callee cleanup, or Directions+Position with caller cleanup before changing Bridge V31.


### Live validation - Bridge V32 accepted repeated walkto commands

Live test after V40 ABI promotion:
- Bridge V32 loaded with profile V40;
- repeated direct AUTO_WALK_TO commands were all accepted by the Ramonia client;
- the previous V31 behavior where only a small subset produced movement is no longer observed;
- this validates the V40 contract in practice: ECX=Game, Directions& plus 12-byte Position value, callee cleanup RET 16;
- Cavebot controller still retains the later safety fixes: one native autoWalk per waypoint, retry only after stuck timeout, max retry limit, and bridge-side cooldown.

Next validation stage: Walk-only Cavebot loop with 2-3 nearby waypoints.


### Bridge V33 - fresh tile actions

Live Cavebot Walk loop remained stable for an extended test period after Bridge V32 / V40.

The next failing area is Cavebot special tile actions (Use/Ladder/Rope/Shovel).

V33 changes:
- direct tile actions force a fresh Tile::things discovery immediately before resolving the target;
- USE_TILE_ITEM_AT and USE_WITH_TILE_AT no longer rely only on a potentially stale Tile cache after approach movement;
- new read-only GET_TILE_ITEMS_AT command lists the actual client item IDs and stack indexes at one x/y/z;
- CromePipeTest adds: tileitems <x> <y> <z> [PID];
- Cavebot Ramonia now requires Bridge V33+ so an older V32 DLL cannot be mistaken for the refreshed-action build.

Validation order:
1. stand adjacent to a ladder/use target;
2. tileitems x y z;
3. usetile x y z exactItemId (or 0 for top item diagnostic);
4. once direct use is confirmed, retest Cavebot Ladder/Use.


### Live validation - direct tile use / bueiro 430

Confirmed live with Bridge V33:
- `CromePipeTest_x64.exe usetile 1268 1284 7 430` successfully activated the bueiro;
- therefore Tile::things target resolution, shared Item holder resolution, dispatcher execution, and Game::use are all working on this object;
- item ID 430 is recorded in the global walker catalog as an open-hole/direct-use object for this validated Ramonia test;
- Cavebot semantic correction: Hole/use_with_ground is now routed through direct `USE_TILE_ITEM_AT`, while Rope and Shovel remain `USE_WITH_TILE_AT` tool actions.

Next validation: Cavebot node `Hole 430 @ 1268, 1284, 7` or generic `Use 430 @ ...` in a short route.


### Ladder follow-up - ID still unresolved

After extended stable Cavebot runtime with no crashes:
- direct Hole/bueiro use is confirmed working;
- ladder did not issue a visible use during the route;
- Look ID 407 seen in a screenshot was initially treated as a ladder candidate, but direct `USE_TILE_ITEM_AT 1282,1258,8 itemId=407` returned `tile_item_not_found`;
- therefore 407 is not promoted as a ladder ID and was removed from the ladder catalog;
- Cavebot direct-use nodes still support category fallback, but only validated/confirmed IDs should be stored there;
- next diagnostic is `GET_TILE_ITEMS_AT` on the exact ladder square, followed by direct `usetile` using the returned item ID.


### Structural tile use policy - itemId 0

Live validation on Ramonia showed:
- `USE_TILE_ITEM_AT 1282,1258,8 itemId=1386` activated the ladder;
- `USE_TILE_ITEM_AT 1282,1258,8 itemId=0` also activated the same ladder;
- GET_TILE_ITEMS_AT showed 1386 as the top item on that tile.

Decision:
- Cavebot Ladder and Hole now prefer `itemId=0`, meaning "use the current top Item on this tile" from a fresh Tile::things snapshot;
- this avoids hard-coding build-specific structure IDs such as Olders 1948 vs Ramonia 1386;
- explicit waypoint/catalog IDs remain fallback diagnostics if top-item use does not work on a special tile;
- Rope/Shovel remain explicit tool actions through USE_WITH_TILE_AT;
- after Ladder/Hole use succeeds, Cavebot waits for GET_PLAYER Z to change before advancing the waypoint.


### Live validation - Cavebot floor transitions up/down

Confirmed live on Ramonia after the top-item tile-use and transition-wait changes:
- Cavebot successfully performed a floor transition upward;
- Cavebot successfully performed a floor transition downward;
- no crash was observed during these transitions;
- structural tile actions now use `USE_TILE_ITEM_AT` with `itemId=0` first, resolving the current top item from a fresh Tile::things snapshot;
- after the use succeeds, Cavebot waits for GET_PLAYER Z to actually change before advancing the waypoint.

This validates the current Ramonia behavior for direct structural Use / Ladder / Hole style transitions without hard-coding one build-specific structure ID.


### Spear pickup tile matching policy

Pick Up Spear must not use top-item semantics.

Policy:
- spear item ID: 2389;
- scan every Item in the tile's Tile::things stack;
- select the exact Item whose ID is 2389 regardless of stackIndex;
- corpses, blood, gold or other objects on the same tile do not block spear detection;
- itemId=0/top-item semantics remain for structural Use/Ladder/Hole, not spear pickup.


### Bridge V34 - local 3x3 spear discovery

Prepared for Pick Up Spear without broad per-cycle tile reads.

Policy:
- feature is intended to poll only while Pick Up Spear is enabled;
- spear ID is 2389;
- search radius is 1 Chebyshev tile: player center + 8 neighbors (maximum 9 tiles);
- exact item ID matching is used at any stackIndex; corpses/other tile objects do not hide the spear;
- the broad Tile discovery remains part of the shared cache refresh only, not each spear poll;
- V34 builds a Tile* -> x/y/z coordinate index when the Tile cache refreshes;
- normal nearby polls use that coordinate index and open Tile::things only for the matching 3x3 tiles.

New read-only diagnostic:
`CromePipeTest_x64.exe nearbyitem 2389 1 [PID]`

Expected response includes `scannedTiles` (normally up to 9), `found`, and exact spear x/y/z/stackIndex when present.

This validates discovery only. Ground -> equipment Game::move destination encoding is still pending one live test before automatic pickup is enabled.


### Bridge V34 live validation - 3x3 spear detection

Live-confirmed on Ramonia:
- Bridge HELLO reported bridgeVersion=34, buildMatched=true and dispatcherReady=true;
- GET_NEARBY_ITEM for spear ID 2389 with radius=1 scanned exactly 9 tiles;
- when the spear was outside the 3x3 window, response returned found=false;
- after the player moved adjacent, the same command returned found=true;
- detected spear: ID 2389 at 1220,1280,7, stackIndex=2, tileThingCount=3;
- therefore exact-ID matching works even when other things share the same tile;
- the feature can remain dormant while Pick Up Spear is disabled and poll only the 3x3 window while enabled.

Next step: validate one Game::move from the exact ground Item holder to one equipment hand slot before enabling automatic pickup.


### Bridge V35 - guarded ground-to-hand pickup test

Prepared after V34 live-confirmed 3x3 exact-ID spear detection.

New diagnostics:
- `CromePipeTest_x64.exe hands` -> reads inventory slots 5 (right) and 6 (left);
- `CromePipeTest_x64.exe pickupitem 2389` -> locates spear ID 2389 only inside the 3x3 window and attempts one Game::move to the selected hand.

Hand policy:
- prefer a hand that already contains the same item ID;
- otherwise choose an empty hand;
- never replace a different equipped item;
- if both hands contain other items, fail closed.

Safety:
- moves exactly count=1 per experimental call;
- source Tile::things holder is revalidated on the game thread;
- source item must still be within Chebyshev distance 1 on the same floor;
- destination is restricted to inventory slot 5 or 6;
- destination encoding under test is OTC inventory Position x=0xFFFF, y=slot, z=0.

This destination encoding is not yet live-confirmed on Ramonia. Automatic Pick Up Spear remains disabled until this one-shot move test succeeds.


### Bridge V35 live validation - hand slots

Live-confirmed on Ramonia through GET_HANDS:
- inventory slot 5 = right hand;
- inventory slot 6 = left hand;
- observed state during validation: right slot 5 occupied by item ID 2512 count 1;
- left slot 6 empty.

This confirms the hand-slot mapping used by the guarded Pick Up Spear test. The next one-shot pickup should preserve item 2512 and choose empty slot 6 for spear ID 2389.


### Bridge V35 live validation - ground spear to free hand

Live-confirmed on Ramonia:
- GET_HANDS confirmed slot 5 right hand and slot 6 left hand;
- test state: right hand contained item ID 2512 count 1 and left hand was empty;
- MOVE_NEARBY_ITEM_TO_HAND correctly rejected spear ID 2389 while it was outside the local 3x3 window with item_not_found_in_3x3;
- after the player moved into range, the same command succeeded;
- source spear: ID 2389 at 1220,1280,7, stackIndex 2;
- Game::move moved exactly count 1 to destination slot 6;
- item 2512 in slot 5 was preserved;
- destination encoding x=0xFFFF, y=inventory slot, z=0 is therefore live-confirmed for ground -> hand on this Ramonia build.

Crome automatic Pick Up Spear is now wired to:
- run only while the Pick Up Spear toggle is enabled;
- search exact configured spear IDs only in the 3x3 player window;
- move one item per cycle;
- prefer a hand already holding the same spear ID, otherwise an empty hand;
- never replace a different item in either hand;
- pause tile polling during detected floor/map transitions.

The requested 20% free-capacity reserve remains configured but is not enforced yet because GET_PLAYER does not currently expose a canonical capacity value. The capacity guard stays explicitly disabled until that field is wired and live-validated.


### Crome automatic Pick Up Spear live validation

Live-confirmed on Ramonia through the Crome UI with Bridge V35:
- Pick Up Spear toggle enabled successfully;
- configured spear ID 2389 was detected only in the local 3x3 player window;
- exact-ID matching worked through Tile::things regardless of stack position;
- ground spear was moved automatically to the correct hand;
- existing item in the opposite hand was preserved;
- repeated automatic pickup behavior worked as intended during the live test;
- no crash was observed.

Current status:
- 3x3 discovery: LIVE CONFIRMED;
- hand slot mapping 5/6: LIVE CONFIRMED;
- ground -> hand Game::move encoding: LIVE CONFIRMED;
- automatic Crome Pick Up Spear: LIVE CONFIRMED;
- 20% free-capacity guard: still pending canonical CAP exposure in GET_PLAYER.


### Pick Up Spear live validation - item covered in tile stack

Additional live confirmation on Ramonia:
- another object was deliberately placed above spear ID 2389 on the same tile;
- automatic Pick Up Spear did not remove or interact with the covering object;
- it still found the spear by exact item ID inside Tile::things and moved only the spear to the hand;
- therefore pickup does not depend on the spear being the top tile item and exact-ID any-stack matching is live-confirmed.


### Bridge V36 - shared uncover tile target

Prepared for the common "remove objects above the real target" behavior used by Pick Up Spear and later Cavebot structure/tool actions.

Behavior prepared:
- exact target Item is located in Tile::things;
- if another Item/corpse is above the target, direct ground -> hand pickup now fails closed with item_covered instead of bypassing the visible stack;
- inspectTileTargetAccess identifies the highest Item blocker above the target;
- UNCOVER_NEARBY_ITEM removes one blocker above an exact nearby item target;
- UNCOVER_TILE_TARGET_AT does the same for an exact item target at a Cavebot coordinate;
- blocker destination is one cached adjacent SQM on the same floor, preferring a tile different from the player's own SQM;
- each call moves only one blocking layer, allowing re-read/revalidation between moves.

Diagnostics:
- CromePipeTest_x64.exe nearbyitem 2389 1
- CromePipeTest_x64.exe uncoveritem 2389
- CromePipeTest_x64.exe pickupitem 2389
- CromePipeTest_x64.exe uncovertile <x> <y> <z> <targetItemId>

Ground -> ground Game::move destination encoding uses normal world Position x/y/z and is EXPERIMENTAL until live-confirmed. Automatic repeated uncover is intentionally not enabled yet.


### V37 uncover ordering evidence

Live Ramonia tile evidence:
- tile 1220,1280,7 contained:
  - ID 103 stackIndex 0
  - ID 16845 stackIndex 1
  - ID 20159 stackIndex 2
  - spear ID 2389 stackIndex 3
- user confirmed ID 20159 was the object visually covering the spear;
- therefore "highest stackIndex = visual blocker" is false for this build;
- this is consistent with classic OTClient top-move semantics walking Tile::things toward the first common/movable item.

V37 changes the experimental uncover candidate for an exact target to the nearest Item before the target in Tile::things. For the validated example, spear 2389 selects blocker 20159.

This is still one-layer experimental behavior. Ground -> ground Game::move must be live-confirmed before automatic uncover is enabled for Pick Up Spear or Cavebot.


### V38 read-only getTopMoveThing ABI diagnostic

Additional live stack evidence superseded the V37 nearest-before-target heuristic:
- base/static-looking prefix remained 103,16845;
- with one user-visible blocker: 20159 appeared before spear 2389;
- after placing another item visually on top, stack became:
  103[0], 16845[1], 2384[2], 20159[3], 2389[4];
- therefore choosing the nearest Item immediately before the target is incorrect for multiple blockers;
- the newest/top visual blocker in this test is 2384, not 20159.

The canonical profile already contains semantic bind:
- Tile::getTopMoveThing RVA 0x197AD0.

V38 adds GET_TOP_MOVE_ABI, which is strictly read-only:
- reads the method bytes;
- reports RET cleanup candidates;
- scans .text for rel32 CALL sites to the method and returns small caller windows;
- does NOT execute getTopMoveThing.

Safety gate:
- MOVE_NEARBY_ITEM_TO_HAND is temporarily blocked with top_move_access_pending_v38;
- UNCOVER_NEARBY_ITEM / UNCOVER_TILE_TARGET_AT are temporarily blocked with top_move_abi_validation_required_v38;
- this prevents the V37 heuristic from moving the wrong item while ABI is being resolved.

Pipe diagnostic:
  CromePipeTest_x64.exe topmoveabi


### V39 getTopMoveThing static ABI evidence

V38 read-only output for semantic bind Tile::getTopMoveThing RVA 0x197AD0 showed:
- method prologue is MSVC x86 frame based;
- ECX is copied at entry, supporting ECX=Tile*;
- [EBP+8] is read immediately and later written through, strongly indicating hidden return-storage for returned ThingPtr;
- method reads Tile+0x24 and Tile+0x28, exactly matching the already validated Tile::things begin/end layout;
- first 160 bytes contained no RET, while an early branch targets method offset +0x1DD, so the prior probe was too short to capture the common epilogue;
- direct rel32 caller scan found no callers, which is compatible with virtual/indirect or registry-driven invocation and is not by itself an ABI failure.

V39 expands the read-only probe to 0x300 bytes and reports:
- thisFromEcx;
- hiddenReturnStorageAtEbp8;
- Tile::things +0x24/+0x28 evidence;
- every C3/C2 return candidate with stack cleanup and surrounding bytes;
- bounded entry and common-return-area windows.

Safety remains fail-closed:
- pickup and uncover stay disabled until return cleanup / returned ThingPtr convention is confirmed.
