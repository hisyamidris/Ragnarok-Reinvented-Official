// Copyright (c) Hercules Dev Team, licensed under GNU GPL.
// See the LICENSE file
// Portions Copyright (c) Athena Dev Teams
#ifndef CONFIG_GENERAL_H
#define CONFIG_GENERAL_H

/**
 * Hercules configuration file (http://herc.ws)
 **/

/**
 * Default Magical Reflection Behavior
 * - When reflecting, reflected damage depends on gears caster is wearing, not target (official)
 * - When disabled damage depends on gears target is wearing, not caster. (old/eathena)
 * @values 1 (enabled) or 0 (disabled)
 **/
#define MAGIC_REFLECTION_TYPE 1

/**
 * Spirit Sphere Limitation
 **/
#define MAX_SPIRITBALL 15

/**
* Spirit Charm Limitation
**/
#define MAX_SPIRITCHARM 10

/**
 * when enabled, reflect damage doesn't bypass devotion (and thus damage is passed to crusader)
 * uncomment to enable
 **/
//#define DEVOTION_REFLECT_DAMAGE

/**
 * No settings past this point
 **/

/**
 * CUSTOM SETTINGS
 **/

/**
 * When enabled, Clown pang voice skill can be cast on friend
 **/
#define CUSTOM_BA_PANGVOICE_FRIEND

/**
 * When enabled, bypass monk/champion combo requirements
 **/
#define CUSTOM_MO_COMBO_BYPASS

/**
 * When enabled, monk/champion combo skills adds 1 sphere
 **/
#define CUSTOM_MO_COMBO_SPIRIT

/**
 * When enabled, use custom monk/champion asura strike
 * Asura strike can be used at any time. 
 * After using Asura Strike SP will not regenerate normally or by items for 3 minutes. 
 * If Asura Strike is used with Explosion Spirits buff and 5 spirit spheres, it will not consume any SP nor will it restrict SP regeneration.
 **/
#define CUSTOM_MO_ASURA

/**
 * When enabled, any damage done on Bleeding target will cause them to take 20% of the next damage received during the duration over 5 seconds
 **/
#define CUSTOM_SC_BLEEDING_DMG

/**
 * When enabled, bleeding damage will end bleeding
 **/
#define CUSTOM_SC_BLEEDING_DMG_REMOVE_SC_BLEEDING

/**
 * When enabled, monk/champion absorb spirit sphere is now a self-card ability. When cast, the monk will consume one spirit sphere and heal based of his ATK.
 **/
#define CUSTOM_MO_ABSORB_SPIRIT

/**
 * When enabled, monk/champion absorb spirit sphere can be use while on bladestop status.
 **/
#define CUSTOM_MO_ABSORB_SPIRIT_BLADESTOP

/**
 * When enabled, Throw Spirit Sphere can also be casted on allied units.
 * This ability will heal allied units based on the skill level and the monk’s ATK. 
 * Just like how the ability used to work, A certain level of Throw Spirit Sphere can use its maximum amount of spirit spheres (equal to the level), 
 * or it could use a fewer amount, if the maximum is not available.
 * Throw Spirit Sphere will work normally on enemy targets.
 **/
#define CUSTOM_MO_FINGEROFFENSIVE

/**
 * When enabled, Blade stop is now a targeted skill. It can be used on both allied and enemy. 
 * While in freeze condition, the monk can cast Absorb Spirit Sphere, Throw Sphere Spirit and Asura Strike. 
 * The duration of the skill depends on the skill level.
 **/
#define CUSTOM_MO_BLADESTOP

#endif // CONFIG_GENERAL_H
