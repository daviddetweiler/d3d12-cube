#pragma once

#include <vector>

#include <gsl/gsl>

namespace helium {
	std::vector<char> load_compiled_shader(gsl::cwzstring<> name);
}
