#ifndef HELIUM_WAVEFRONT_LOADER_H
#define HELIUM_WAVEFRONT_LOADER_H

#include <array>
#include <vector>

#include <gsl/gsl>

namespace cube {
	struct vector3 {
		float x;
		float y;
		float z;
	};

	struct face_descriptor {
		std::array<unsigned int, 3> indices;
	};

	struct wavefront {
		std::vector<vector3> positions;
		std::vector<face_descriptor> faces;
	};

	wavefront load_wavefront(gsl::czstring name);
}

#endif
