#pragma once

#include <vector>

#include <gsl/gsl>

namespace helium {
	struct vector3 {
		float x;
		float y;
		float z;
	};

	struct wavefront_vertex {
		unsigned int position;
		unsigned int normal;
		unsigned int uvw;
	};

	struct triangle {
		std::array<wavefront_vertex, 3> vertices;
	};

	struct wavefront_object {
		std::vector<vector3> positions;
		std::vector<vector3> normals;
		std::vector<vector3> uvws;
		std::vector<triangle> faces;
	};

	wavefront_object load_wavefront_object(gsl::czstring<> name);
}
