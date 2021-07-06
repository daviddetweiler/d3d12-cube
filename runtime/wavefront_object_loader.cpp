#include "wavefront_object_loader.h"

#include <charconv>
#include <exception>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include <gsl/gsl>

#include <Windows.h>

namespace helium {
	namespace {
		template <char delimiter, typename iterator_type>
		void eat_delimiters(iterator_type& iterator, const iterator_type& last) noexcept
		{
			while (iterator != last && *iterator == delimiter)
				++iterator;
		}

		template <char delimiter, typename iterator_type>
		void advance_to_delimiter(iterator_type& iterator, const iterator_type& last) noexcept
		{
			while (iterator != last && *iterator != delimiter)
				++iterator;
		}

		template <char delimiter = ' ', typename iterator_type>
		std::string_view get_next(iterator_type& iterator, const iterator_type& last)
		{
			eat_delimiters<delimiter>(iterator, last);
			const iterator_type range_first {iterator};
			if (range_first == last)
				return {};

			advance_to_delimiter<delimiter>(iterator, last);
			const iterator_type range_last {iterator};

			return {&*range_first, gsl::narrow<std::size_t>(range_last - range_first)};
		}

		template <typename type>
		type convert(std::string_view string)
		{
			type value {};
			const auto last = std::next(string.data(), string.size());
			const std::from_chars_result result {
				std::from_chars(string.data(), std::next(string.data(), string.size()), value)};

			Ensures(result.ptr == last);

			return value;
		}

		wavefront_vertex unpack_face_vertex(std::string_view string)
		{
			auto iterator = string.begin();
			const auto last = string.end();
			const auto vertex = convert<unsigned int>(get_next<'/'>(iterator, last));
			const auto texture = convert<unsigned int>(get_next<'/'>(iterator, last));
			const auto normal = convert<unsigned int>(get_next<'/'>(iterator, last));
			if (!get_next<'/'>(iterator, last).empty())
				std::terminate();

			return {vertex, normal, texture};
		}
	}
}

helium::wavefront_object helium::load_wavefront_object(gsl::czstring<> name)
{
	std::ifstream file {name, file.ate};
	file.exceptions(file.badbit);
	std::vector<char> content(file.tellg());
	file.seekg(file.beg);
	file.read(content.data(), content.size());

	std::vector<vector3> vertices {};
	std::vector<vector3> normals {};
	std::vector<vector3> uvws {};
	std::vector<triangle> faces {};

	auto content_iterator = content.begin();
	const auto content_last = content.end();
	while (content_iterator != content_last) {
		const auto line = get_next<'\n'>(content_iterator, content_last);
		auto iterator = line.begin();
		const auto last = line.end();
		const auto command = get_next(iterator, last);
		if (command == "v") {
			const auto x = convert<float>(get_next(iterator, last));
			const auto y = convert<float>(get_next(iterator, last));
			const auto z = convert<float>(get_next(iterator, last));
			if (!get_next(iterator, last).empty())
				std::terminate();

			vertices.push_back({x, y, z});
		}
		else if (command == "vn") {
			const auto x = convert<float>(get_next(iterator, last));
			const auto y = convert<float>(get_next(iterator, last));
			const auto z = convert<float>(get_next(iterator, last));
			if (!get_next(iterator, last).empty())
				std::terminate();

			normals.push_back({x, y, z});
		}
		else if (command == "vt") {
			const auto u = convert<float>(get_next(iterator, last));
			const auto v = convert<float>(get_next(iterator, last));
			const auto w = convert<float>(get_next(iterator, last));
			const auto tail = get_next(iterator, last);
			if (!tail.empty())
				std::terminate();

			uvws.push_back({u, v, w});
		}
		else if (command == "f") {
			const triangle face {
				{unpack_face_vertex(get_next(iterator, last)),
				 unpack_face_vertex(get_next(iterator, last)),
				 unpack_face_vertex(get_next(iterator, last))}};

			if (!get_next(iterator, last).empty())
				std::terminate();

			faces.push_back(face);
		}
	}

	return {vertices, normals, uvws, faces};
}
