#include "wavefront_loader.h"

#include <charconv>
#include <cstddef>
#include <fstream>
#include <iterator>
#include <string_view>
#include <utility>
#include <vector>

#include <gsl/gsl>

namespace helium {
	namespace {
		template <char delimiter, typename iterator_type>
		std::string_view get_next(iterator_type& iterator, const iterator_type& last) noexcept
		{
			for (; iterator != last && *iterator == delimiter; ++iterator)
				;

			const iterator_type first_char {iterator};
			for (; iterator != last && *iterator != delimiter; ++iterator)
				;

			const iterator_type last_char {iterator};

			if (first_char == last_char)
				return {};

			return {&*first_char, gsl::narrow_cast<std::size_t>(last_char - first_char)};
		}

		template <typename type>
		type convert(std::string_view string)
		{
			type value {};
			std::from_chars(string.data(), std::next(string.data(), string.size()), value);
			return value;
		}
	}
}

helium::wavefront helium::load_wavefront(gsl::czstring<> name)
{
	std::ifstream file {name, file.ate};
	file.exceptions(file.badbit);
	std::vector<char> content(file.tellg());
	file.seekg(file.beg);
	file.read(content.data(), content.size());

	auto content_iterator = content.begin();
	const auto content_end = content.end();

	std::vector<vector3> positions {};
	std::vector<face_descriptor> faces {};
	while (true) {
		const auto line = get_next<'\n'>(content_iterator, content_end);
		if (line.empty())
			break;

		const auto line_end = line.end();
		auto line_iterator = line.begin();
		const auto type = get_next<' '>(line_iterator, line_end);
		if (type == "v") {
			const auto x = get_next<' '>(line_iterator, line_end);
			const auto y = get_next<' '>(line_iterator, line_end);
			const auto z = get_next<' '>(line_iterator, line_end);
			positions.push_back({convert<float>(x), convert<float>(y), convert<float>(z)});
		}
		else if (type == "f") {
			faces.push_back({
				convert<unsigned int>(get_next<' '>(line_iterator, line_end)),
				convert<unsigned int>(get_next<' '>(line_iterator, line_end)),
				convert<unsigned int>(get_next<' '>(line_iterator, line_end)),
			});
		}
	}

	return {.positions {std::move(positions)}, .faces {std::move(faces)}};
}
