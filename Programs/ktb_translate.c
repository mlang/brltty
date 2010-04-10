/*
 * BRLTTY - A background process providing access to the console screen (when in
 *          text mode) for a blind person using a refreshable braille display.
 *
 * Copyright (C) 1995-2010 by The BRLTTY Developers.
 *
 * BRLTTY comes with ABSOLUTELY NO WARRANTY.
 *
 * This is free software, placed under the terms of the
 * GNU General Public License, as published by the Free Software
 * Foundation; either version 2 of the License, or (at your option) any
 * later version. Please see the file LICENSE-GPL for details.
 *
 * Web Page: http://mielke.cc/brltty/
 *
 * This software is maintained by Dave Mielke <dave@mielke.cc>.
 */

#include "prologue.h"

#include <stdio.h>
#include <string.h>

#include "log.h"
#include "ktb.h"
#include "ktb_internal.h"
#include "ktb_inspect.h"
#include "brl.h"

static int
searchKeyBinding (const void *target, const void *element) {
  const KeyBinding *reference = target;
  const KeyBinding *const *binding = element;
  return compareKeyBindings(reference, *binding);
}

static const KeyBinding *
findKeyBinding (KeyTable *table, unsigned char context, const KeyValue *immediate, int *isIncomplete) {
  const KeyContext *ctx = getKeyContext(table, context);

  if (ctx && ctx->sortedKeyBindings &&
      (table->pressedCount <= MAX_MODIFIERS_PER_COMBINATION)) {
    KeyBinding target;
    memset(&target, 0, sizeof(target));

    if (immediate) {
      target.combination.immediateKey = *immediate;
      target.combination.flags |= KCF_IMMEDIATE_KEY;
    }

    target.combination.modifierCount = table->pressedCount;
    memcpy(target.combination.modifierKeys, table->pressedKeys,
           target.combination.modifierCount * sizeof(target.combination.modifierKeys[0]));

    {
      int index;

      for (index=0; index<target.combination.modifierCount; index+=1) {
        KeyValue *modifier = &target.combination.modifierKeys[index];
        if (modifier->set) modifier->key = KTB_KEY_MAX;
      }
    }

    if (target.combination.flags & KCF_IMMEDIATE_KEY)
      if (target.combination.immediateKey.set)
        target.combination.immediateKey.key = KTB_KEY_MAX;

    {
      const KeyBinding **binding = bsearch(&target, ctx->sortedKeyBindings, ctx->keyBindingCount, sizeof(*ctx->sortedKeyBindings), searchKeyBinding);
      if (binding) {
        if ((*binding)->command != EOF) return *binding;
        *isIncomplete = 1;
      }
    }
  }

  return NULL;
}

static int
searchHotkeyEntry (const void *target, const void *element) {
  const HotkeyEntry *reference = target;
  const HotkeyEntry *const *hotkey = element;
  return compareKeyValues(&reference->keyValue, &(*hotkey)->keyValue);
}

static const HotkeyEntry *
findHotkeyEntry (KeyTable *table, unsigned char context, const KeyValue *keyValue) {
  const KeyContext *ctx = getKeyContext(table, context);

  if (ctx && ctx->sortedHotkeyEntries) {
    HotkeyEntry target = {
      .keyValue = *keyValue
    };

    {
      const HotkeyEntry **hotkey = bsearch(&target, ctx->sortedHotkeyEntries, ctx->hotkeyCount, sizeof(*ctx->sortedHotkeyEntries), searchHotkeyEntry);
      if (hotkey) return *hotkey;
    }
  }

  return NULL;
}

static int
makeKeyboardCommand (KeyTable *table, unsigned char context) {
  int chordsRequested = context == BRL_CTX_CHORDS;
  const KeyContext *ctx;

  if (chordsRequested) context = table->persistentContext;
  if (!(ctx = getKeyContext(table, context))) return EOF;

  if (ctx->keyMap) {
    int keyboardCommand = BRL_BLK_PASSDOTS;
    int dotPressed = 0;
    int spacePressed = 0;

    {
      unsigned int pressedIndex;

      for (pressedIndex=0; pressedIndex<table->pressedCount; pressedIndex+=1) {
        const KeyValue *keyValue = &table->pressedKeys[pressedIndex];
        KeyboardFunction function = ctx->keyMap[keyValue->key];

        if (keyValue->set) return EOF;
        if (function == KBF_None) return EOF;

        {
          const KeyboardFunctionEntry *kbf = &keyboardFunctionTable[function];

          keyboardCommand |= kbf->bit;

          if (!kbf->bit) {
            spacePressed = 1;
          } else if (kbf->bit & BRL_MSK_ARG) {
            dotPressed = 1;
          }
        }
      }

      if (dotPressed) keyboardCommand |= ctx->superimposedBits;
    }

    if (chordsRequested && spacePressed) {
      keyboardCommand |= BRL_DOTC;
    } else if (dotPressed == spacePressed) {
      return EOF;
    }

    return keyboardCommand;
  }

  return EOF;
}

static int
processCommand (KeyTable *table, int command) {
  int blk = command & BRL_MSK_BLK;
  int arg = command & BRL_MSK_ARG;

  switch (blk) {
    case BRL_BLK_CONTEXT:
      if (!BRL_DELAYED_COMMAND(command)) {
        unsigned char context = BRL_CTX_DEFAULT + arg;
        const KeyContext *ctx = getKeyContext(table, context);

        if (ctx) {
          table->currentContext = context;
          if (!isTemporaryKeyContext(table, ctx)) table->persistentContext = context;
        }
      }

      command = BRL_CMD_NOOP;
      break;

    default:
      break;
  }

  return enqueueCommand(command);
}

static int
findPressedKey (KeyTable *table, const KeyValue *value, unsigned int *position) {
  return findKeyValue(table->pressedKeys, table->pressedCount, value, position);
}

static int
insertPressedKey (KeyTable *table, const KeyValue *value, unsigned int position) {
  return insertKeyValue(&table->pressedKeys, &table->pressedCount, &table->pressedSize, value, position);
}

static void
removePressedKey (KeyTable *table, unsigned int position) {
  removeKeyValue(table->pressedKeys, &table->pressedCount, position);
}

KeyTableState
processKeyEvent (KeyTable *table, unsigned char context, unsigned char set, unsigned char key, int press) {
  KeyValue keyValue = {
    .set = set,
    .key = key
  };
  unsigned int keyPosition;

  KeyTableState state = KTS_UNBOUND;
  int command = EOF;
  int immediate = 1;
  const HotkeyEntry *hotkey;

  if (context == BRL_CTX_DEFAULT) context = table->currentContext;
  if (press) table->currentContext = table->persistentContext;

  if ((hotkey = findHotkeyEntry(table, context, &keyValue))) {
    int cmd = press? hotkey->pressCommand: hotkey->releaseCommand;
    if (cmd != BRL_CMD_NOOP) processCommand(table, (command = cmd));
    state = KTS_HOTKEY;
  } else {
    if (findPressedKey(table, &keyValue, &keyPosition)) removePressedKey(table, keyPosition);

    if (press) {
      int isIncomplete = 0;
      const KeyBinding *binding = findKeyBinding(table, context, &keyValue, &isIncomplete);
      insertPressedKey(table, &keyValue, keyPosition);

      if (binding) {
        command = binding->command;
      } else if ((binding = findKeyBinding(table, context, NULL, &isIncomplete))) {
        command = binding->command;
        immediate = 0;
      } else if ((command = makeKeyboardCommand(table, context)) != EOF) {
        immediate = 0;
      } else if (context == BRL_CTX_DEFAULT) {
        command = EOF;
      } else {
        removePressedKey(table, keyPosition);
        binding = findKeyBinding(table, BRL_CTX_DEFAULT, &keyValue, &isIncomplete);
        insertPressedKey(table, &keyValue, keyPosition);

        if (binding) {
          command = binding->command;
        } else if ((binding = findKeyBinding(table, BRL_CTX_DEFAULT, NULL, &isIncomplete))) {
          command = binding->command;
          immediate = 0;
        } else {
          command = EOF;
        }
      }

      if (command == EOF) {
        if (isIncomplete) state = KTS_MODIFIERS;

        if (table->command != EOF) {
          table->command = EOF;
          processCommand(table, (command = BRL_CMD_NOOP));
        }
      } else {
        if (command != table->command) {
          table->command = command;

          if (binding->flags & KBF_ADJUST) {
            int index;

            for (index=0; index<table->pressedCount; index+=1) {
              const KeyValue *pressed = &table->pressedKeys[index];

              if (pressed->set) {
                command += pressed->key;
                break;
              }
            }
          }

          if ((table->immediate = immediate)) {
            command |= BRL_FLG_REPEAT_INITIAL | BRL_FLG_REPEAT_DELAY;
          } else {
            command |= BRL_FLG_REPEAT_DELAY;
          }

          processCommand(table, command);
        } else {
          command = EOF;
        }

        state = KTS_COMMAND;
      }
    } else {
      if (table->command != EOF) {
        if (table->immediate) {
          command = BRL_CMD_NOOP;
        } else {
          command = table->command;
        }

        table->command = EOF;
        processCommand(table, command);
      }
    }
  }

  if (table->logKeyEvents) {
    char buffer[0X40];
    size_t size = sizeof(buffer);
    int offset = 0;
    int length;

    snprintf(&buffer[offset], size, "Key %s: Ctx:%u Set:%u Key:%u%n",
             press? "Press": "Release",
             context, set, key, &length);
    offset += length, size -= length;

    if (command != EOF) {
      snprintf(&buffer[offset], size, " Cmd:%06X%n", command, &length);
      offset += length, size -= length;
    }

    LogPrint(LOG_DEBUG, "%s", buffer);
  }

  return state;
}

void
logKeyEvents (KeyTable *table) {
  table->logKeyEvents = 1;
}
