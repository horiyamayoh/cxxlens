#include <algorithm>
#include <memory>
#include <set>
#include <string>
#include <utility>

#include <cxxlens/testing.hpp>

#include "../core/canonical_json.hpp"
#include "workspace_fixture_data.hpp"

namespace cxxlens::testing
{
	namespace
	{
		using cxxlens::detail::json::json_value;

		[[nodiscard]] error fixture_error(std::string reason, std::string field = {})
		{
			error failure;
			failure.code.value = "testing.fixture-invalid";
			failure.message = "workspace fixture is invalid";
			failure.attributes.emplace("reason", std::move(reason));
			if (!field.empty())
				failure.attributes.emplace("field", std::move(field));
			return failure;
		}

		[[nodiscard]] bool safe_relative_path(const path& value)
		{
			if (value.empty() || value.is_absolute())
				return false;
			const auto normalized = value.lexically_normal();
			return !normalized.empty() && *normalized.begin() != "..";
		}

		[[nodiscard]] std::string normalized_path(const path& value)
		{
			return value.lexically_normal().generic_string();
		}

		[[nodiscard]] json_value::array strings(const std::vector<std::string>& values)
		{
			json_value::array output;
			for (const auto& value : values)
				output.emplace_back(value);
			return output;
		}

		[[nodiscard]] std::shared_ptr<detail::workspace_fixture_data>
		copy_data(const std::shared_ptr<const detail::workspace_fixture_data>& input)
		{
			return std::make_shared<detail::workspace_fixture_data>(*input);
		}

		void sort_bundle(fixture_bundle& bundle)
		{
			std::ranges::sort(bundle.files,
							  {},
							  [](const fixture_file& file)
							  {
								  return normalized_path(file.name);
							  });
			std::ranges::sort(bundle.variants, {}, &fixture_variant::name);
		}
	} // namespace

	result<void> fixture_bundle::validate() const
	{
		if (!root.is_absolute())
			return fixture_error("root-not-absolute", "root");
		if (!safe_relative_path(main_file))
			return fixture_error("unsafe-main-file", "main_file");
		if (language != "c" && language != "c++")
			return fixture_error("unsupported-language", "language");
		if (standard.empty() || target.empty())
			return fixture_error("toolchain-metadata-empty");
		std::set<std::string> file_names;
		for (const auto& file : files)
		{
			if (!safe_relative_path(file.name))
				return fixture_error("unsafe-file-path", normalized_path(file.name));
			if (!file_names.insert(normalized_path(file.name)).second)
				return fixture_error("duplicate-file", normalized_path(file.name));
		}
		if (!file_names.contains(normalized_path(main_file)))
			return fixture_error("main-file-missing", normalized_path(main_file));
		std::set<std::string> variant_names;
		for (const auto& variant : variants)
		{
			if (variant.name.empty() || !variant_names.insert(variant.name).second)
				return fixture_error("duplicate-or-empty-variant", variant.name);
			if (std::ranges::any_of(variant.arguments, &std::string::empty))
				return fixture_error("empty-variant-argument", variant.name);
		}
		if (compilation_database_json.empty())
			return fixture_error("compile-database-empty");
		return {};
	}

	std::string fixture_bundle::to_json() const
	{
		if (!validate())
			return {};
		auto canonical_files = files;
		std::ranges::sort(canonical_files,
						  {},
						  [](const fixture_file& file)
						  {
							  return normalized_path(file.name);
						  });
		auto canonical_variants = variants;
		std::ranges::sort(canonical_variants, {}, &fixture_variant::name);
		json_value::array file_rows;
		for (const auto& file : canonical_files)
			file_rows.emplace_back(json_value::object{
				{"content", file.content},
				{"generated", file.generated},
				{"kind", file.kind == fixture_file_kind::source ? "source" : "header"},
				{"path", normalized_path(file.name)},
				{"system_header", file.system_header},
			});
		json_value::array variant_rows;
		for (const auto& variant : canonical_variants)
		{
			json_value::object environment;
			for (const auto& [name, value] : variant.environment)
				environment.emplace_back(name, value);
			variant_rows.emplace_back(json_value::object{
				{"arguments", strings(variant.arguments)},
				{"environment", std::move(environment)},
				{"name", variant.name},
			});
		}
		auto document =
			cxxlens::detail::json::envelope({"cxxlens.testing.fixture.v1"},
											{{"arguments", strings(arguments)},
											 {"compilation_database", compilation_database_json},
											 {"files", std::move(file_rows)},
											 {"language", language},
											 {"main_file", normalized_path(main_file)},
											 {"standard", standard},
											 {"target", target},
											 {"variants", std::move(variant_rows)}});
		auto output = cxxlens::detail::json::write(document);
		return output ? std::move(output.value()) : std::string{};
	}

	workspace_fixture::workspace_fixture(std::shared_ptr<const detail::workspace_fixture_data> data)
		: data_{std::move(data)}
	{
	}

	workspace_fixture workspace_fixture::cpp(std::string main_source)
	{
		auto data = std::make_shared<detail::workspace_fixture_data>();
		data->language = "c++";
		data->main_file = "main.cpp";
		data->main_source = std::move(main_source);
		data->standard = "c++23";
		data->target = "x86_64-unknown-linux-gnu";
		return workspace_fixture{std::move(data)};
	}

	workspace_fixture workspace_fixture::c(std::string main_source)
	{
		auto data = std::make_shared<detail::workspace_fixture_data>();
		data->language = "c";
		data->main_file = "main.c";
		data->main_source = std::move(main_source);
		data->standard = "c23";
		data->target = "x86_64-unknown-linux-gnu";
		return workspace_fixture{std::move(data)};
	}

	workspace_fixture workspace_fixture::main_file(path value) const
	{
		auto data = copy_data(data_);
		data->main_file = std::move(value);
		return workspace_fixture{std::move(data)};
	}

	workspace_fixture workspace_fixture::add_file(path name, std::string content) const
	{
		auto data = copy_data(data_);
		data->files.push_back({std::move(name), std::move(content), fixture_file_kind::source});
		return workspace_fixture{std::move(data)};
	}

	workspace_fixture workspace_fixture::add_header(path name, std::string content) const
	{
		auto data = copy_data(data_);
		data->files.push_back({std::move(name), std::move(content), fixture_file_kind::header});
		return workspace_fixture{std::move(data)};
	}

	workspace_fixture workspace_fixture::add_variant(fixture_variant value) const
	{
		auto data = copy_data(data_);
		data->variants.push_back(std::move(value));
		return workspace_fixture{std::move(data)};
	}

	workspace_fixture workspace_fixture::standard(std::string value) const
	{
		auto data = copy_data(data_);
		data->standard = std::move(value);
		return workspace_fixture{std::move(data)};
	}

	workspace_fixture workspace_fixture::target(std::string triple) const
	{
		auto data = copy_data(data_);
		data->target = std::move(triple);
		return workspace_fixture{std::move(data)};
	}

	workspace_fixture workspace_fixture::argument(std::string value) const
	{
		auto data = copy_data(data_);
		data->arguments.push_back(std::move(value));
		return workspace_fixture{std::move(data)};
	}

	workspace_fixture workspace_fixture::generated(path value) const
	{
		auto data = copy_data(data_);
		data->generated_files.insert(std::move(value));
		return workspace_fixture{std::move(data)};
	}

	workspace_fixture workspace_fixture::system_header(path value) const
	{
		auto data = copy_data(data_);
		data->system_headers.insert(std::move(value));
		return workspace_fixture{std::move(data)};
	}

	result<fixture_bundle> workspace_fixture::materialize(const path& root) const
	{
		if (!data_)
			return fixture_error("empty-builder");
		fixture_bundle bundle;
		bundle.root = root.lexically_normal();
		bundle.main_file = data_->main_file.lexically_normal();
		bundle.language = data_->language;
		bundle.standard = data_->standard;
		bundle.target = data_->target;
		bundle.arguments = data_->arguments;
		bundle.files = data_->files;
		bundle.files.push_back({bundle.main_file, data_->main_source, fixture_file_kind::source});
		bundle.variants = data_->variants;
		if (bundle.variants.empty())
			bundle.variants.push_back({"default", {}, {}});
		for (auto& file : bundle.files)
		{
			file.name = file.name.lexically_normal();
			file.generated = data_->generated_files.contains(file.name);
			file.system_header = data_->system_headers.contains(file.name);
		}
		sort_bundle(bundle);

		json_value::array commands;
		for (const auto& variant : bundle.variants)
		{
			std::vector<std::string> argv{
				bundle.language == "c++" ? "clang++" : "clang",
				"-std=" + bundle.standard,
				"--target=" + bundle.target,
			};
			argv.insert(argv.end(), bundle.arguments.begin(), bundle.arguments.end());
			argv.insert(argv.end(), variant.arguments.begin(), variant.arguments.end());
			argv.push_back(normalized_path(bundle.main_file));
			commands.emplace_back(json_value::object{
				{"arguments", strings(argv)},
				{"directory", "."},
				{"file", normalized_path(bundle.main_file)},
				{"output", "build/" + variant.name + "/main.o"},
			});
		}
		auto database = cxxlens::detail::json::write(commands);
		if (!database)
			return std::move(database.error());
		bundle.compilation_database_json = std::move(database.value());
		if (auto status = bundle.validate(); !status)
			return std::move(status.error());
		return bundle;
	}
} // namespace cxxlens::testing
