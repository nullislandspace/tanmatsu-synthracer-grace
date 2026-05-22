#include "attachments.h"

char const* attachment_name(attachment_id_t id) {
    switch (id) {
        case ATTACH_MAGNET:  return "Magnet";
        case ATTACH_BATTERY: return "Battery";
        case ATTACH_NONE:
        default:             return "[empty]";
    }
}
