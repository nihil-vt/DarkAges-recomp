#pragma once

// Native reimplementations of game functions that the recompiler cannot
// generate. The N64Recomp "ignored" mechanism renames such functions to
// <name>_recomp and only emits a declaration, so the implementations live
// here.
