// Registers the recompiled code sections with the runtime so that the
// recomp can look up functions by their address in the original ROM.

#include "mace_overlays.h"

#include "librecomp/overlays.hpp"

#include "recomp_overlays.inl"

namespace mace {

void register_overlays() {
    recomp::overlays::overlay_section_table_data_t sections{
        .code_sections = section_table,
        .num_code_sections = ARRLEN(section_table),
        .total_num_sections = num_sections,
    };

    recomp::overlays::overlays_by_index_t overlays{
        .table = overlay_sections_by_index,
        .len = ARRLEN(overlay_sections_by_index),
    };

    recomp::overlays::register_overlays(sections, overlays);
}

} // namespace mace
