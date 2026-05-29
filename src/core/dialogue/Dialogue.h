#pragma once

//------------------------------------------------------------------------------
// Claymore Dialogue System
//
// A native dialogue system for creating and playing character conversations.
//
// Components:
// - DialogueTypes.h     : Core data structures (commands, choices, events)
// - DialogueParser.h    : Parse .dlg script files into conversation trees
// - DialogueLibrary.h   : Asset type for character dialogue libraries
// - DialogueManager.h   : Runtime dialogue playback and control
//
// Usage:
//
// 1. Create a DialogueLibrary asset with conversation entries
// 2. Reference the library from a ClayScriptableObject (e.g., CharacterData)
// 3. Use DialogueManager to play conversations at runtime
//
// Dialogue Script Syntax (.dlg files):
//
//   {Conversation Title}
//   
//   (emote)                          # Animation/camera cue
//   <condition>                      # Condition block
//   @labelName                       # Label for goto
//   
//   Speaker: Dialogue text           # Dialogue line
//       - Choice 1                   # Player choice
//           Response to choice 1    # Indented response
//       - [condition] Choice 2      # Conditional choice
//           Response to choice 2
//   
//   \SetState(key, value)           # Set game state
//   \SetEmotion(happy)              # Set character emotion
//   \goto(labelName)                # Jump to label
//   \end                            # End conversation
//   
//   # Extended commands:
//   \GiveItem(itemId, count)
//   \TakeItem(itemId, count)
//   \StartQuest(questId)
//   \CompleteStep(questId, stepId)
//   \PlayAnim(animName)
//   \PlaySound(path)
//   \Wait(seconds)
//   \Camera(action, target)
//
// Example:
//
//   {Tavern Keeper Greeting}
//   
//   (wave)
//   Keeper: Welcome to the Silver Tankard!
//   Keeper: What can I get for you?
//       - I'll have an ale.
//           \TakeItem(Gold, 5)
//           \GiveItem(Ale, 1)
//           Keeper: Here you go, friend.
//       - [HasQuest:FindThief] I'm looking for someone.
//           Keeper: Ah, you mean that shady fellow?
//           \SetState(ThiefLocation, Back_Room)
//           Keeper: He's in the back room.
//       - Nothing, thanks.
//           Keeper: Let me know if you need anything.
//   
//   Keeper: Enjoy your stay!
//   \end
//
//------------------------------------------------------------------------------

#include "DialogueTypes.h"
#include "DialogueParser.h"
#include "DialogueLibrary.h"
#include "DialogueManager.h"

