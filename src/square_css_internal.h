/* =====================================================================
 *  Square Browser Engine — square_css_internal.h
 *  Shared declarations between the CSS modules:
 *    square_css.cpp             stylesheet / selector / media-query parsing
 *    square_css_selectors.cpp   selector matching
 * ===================================================================== */
#pragma once

#include "square_content_internal.h"

#include <string>
#include <vector>

namespace sqrl {

/* defined in square_css.cpp, used by selector matching (:not/:is/:where) */
void split_selector_list(const std::string& in, std::vector<std::string>* out);
bool parse_selector(const std::string& in, Selector* out);

} /* namespace sqrl */
