# Title screen and boot logos

The Bandai Namco and tri-Crescendo logos are state 1 of `TITLE_TASK`, drawn
from `title.bmd`.

## The task

Constructed by `sub_82132F78`, set up by `sub_82133130` (`TITLE_TASK__Init`),
which writes the entry state to `+376`: `1` (logos) while `byte_822F4242` is
set, which is first boot only, else `2` (menu) with the cursor in `+10644`.

`sub_82133CA0` ticks it off the state at `+372`:

| State | What runs |
| --- | --- |
| 0 | Waits for `title.bmd`, then `sub_821333C0(task, task[+376])` |
| 1 | `sub_821338E0`, the logo script |
| 2 | `sub_82133A28`, the menu |
| 3, 5 | Countdowns on `+384` after a menu choice |
| 4 | Our hook only, while the Japanese title file reloads |

`sub_821333C0` enters a state. State 1 loads `title.bmd` entries 0 and 1 as
sprites and sets the script cursor to `0x82081AE0`. State 2 frees the effect
at `+10640`, builds the menu from entry 3, starts `sound\cxs\mp101.cxs` and
loads the title event.

## The logo script

| Field | Meaning |
| --- | --- |
| `+380` | Cursor into the u16 bytecode |
| `+384` | Frames left on the current wait |
| `+388` | Fade alpha (float) |
| `+392` | Fade alpha step per frame (float) |
| `+396` | Fade color |
| `+456` | Sprite drawn this frame, 0 for none |

`sub_821335E0(task, fast)` runs one op (table at `byte_82082180`) and returns
nonzero when stepping should stop for the frame.

| Op | Arg | Effect |
| --- | --- | --- |
| 0 | slot | Show sprite slot `arg` |
| 1 | entry | Wait for `byte_82440848` (rewinds, returns 1), clear `+456`, build `title.bmd` entry `arg` into `+10640` |
| 2 | frames | `+384 = arg` |
| 3, 4 | frames | Fade with black |
| 5, 6 | frames | Fade with white |
| 7 | 2 × u16 | Set an effect label (`sub_820CCE28`) |
| 8 | | Yield, returns 1 |
| 9 | state | `sub_821333C0(task, arg)`, returns 1 |

The retail script ends with `9 2`. `byte_82440848` is set by the task
scheduler (`sub_8212DA18`, `sub_82132B28`) when boot loading is done.

`sub_821338E0` steps the script when `+384` runs out. Start (bit 4 of the pad
words at `dword_824BB5C4`) zeroes `+384` and steps with `fast` set, which
zeroes each wait, until a step returns nonzero.

## skip_intro

Handled in the `sub_82133CA0` hook in `src/core/game_settings.cpp`. In state 1
it steps with `fast` set until the state changes, through the yields, since
each draws a logo. If a step leaves `+380` unchanged, op 1 is waiting: it
clears `+456`, `+388` and `+392` so the frame is black rather than the
script's white, and retries next frame.
