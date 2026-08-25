#include <algorithm>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

#include "doctor_product.hpp"

namespace
{
	using cxxlens::sdk::doctor::canonical_json;
	using cxxlens::sdk::doctor::markdown_projection;
	using cxxlens::sdk::doctor::product_error;

	void print_error(const product_error& error)
	{
		std::cerr << error.code;
		if (!error.field.empty())
			std::cerr << " field=" << error.field;
		if (!error.detail.empty())
			std::cerr << " detail=" << error.detail;
		std::cerr << '\n';
	}

	[[nodiscard]] bool print_document(const std::string_view format, const std::string_view json)
	{
		if (format == "json")
			std::cout << json << '\n';
		else if (format == "markdown")
			std::cout << markdown_projection(json);
		else
			return false;
		return true;
	}

	[[nodiscard]] int run_relation_presence(const int argc, char** argv)
	{
		std::vector<std::string_view> relation_ids;
		std::string_view format = "json";
		bool format_seen{};
		for (int index = 2; index < argc; ++index)
		{
			const std::string_view argument{argv[index]};
			if (argument == "--format")
			{
				if (format_seen)
				{
					std::cerr << "doctor.relation-request-invalid field=format "
								 "detail=duplicate-option\n";
					return 2;
				}
				format_seen = true;
				if (++index >= argc)
				{
					std::cerr
						<< "doctor.relation-request-invalid field=format detail=missing-value\n";
					return 2;
				}
				format = argv[index];
				continue;
			}
			if (argument.rfind("--format=", 0U) == 0U)
			{
				if (format_seen)
				{
					std::cerr << "doctor.relation-request-invalid field=format "
								 "detail=duplicate-option\n";
					return 2;
				}
				format_seen = true;
				format = argument.substr(9U);
				continue;
			}
			if (argument.empty() || argument.front() == '-')
			{
				std::cerr << "doctor.relation-request-invalid field=relation "
							 "detail=unexpected-argument\n";
				return 2;
			}
			if (argument.size() > cxxlens::sdk::doctor::maximum_json_string_bytes)
			{
				std::cerr << "doctor.relation-request-invalid field=relation detail=byte-limit\n";
				return 2;
			}
			if (relation_ids.size() >= cxxlens::sdk::doctor::maximum_json_collection_count)
			{
				std::cerr << "doctor.relation-request-invalid field=relation detail=count-limit\n";
				return 2;
			}
			relation_ids.push_back(argument);
		}
		if (format != "json" && format != "markdown")
		{
			std::cerr << "doctor.relation-request-invalid field=format detail=unsupported\n";
			return 2;
		}
		const auto checked = cxxlens::sdk::doctor::check_relations(relation_ids);
		if (std::holds_alternative<product_error>(checked))
		{
			print_error(std::get<product_error>(checked));
			return 2;
		}
		const cxxlens::sdk::doctor::installed_product_catalog_loader catalog_loader;
		auto loaded_catalog = catalog_loader.load();
		if (std::holds_alternative<product_error>(loaded_catalog))
		{
			print_error(std::get<product_error>(loaded_catalog));
			return 2;
		}
		const auto& checks = std::get<std::vector<cxxlens::sdk::doctor::relation_check>>(checked);
		const auto json = canonical_json(cxxlens::sdk::doctor::to_json(
			checks,
			std::get<cxxlens::sdk::doctor::authenticated_capability_catalog>(
				std::move(loaded_catalog))));
		if (!print_document(format, json))
			return 2;
		return std::ranges::all_of(checks,
								   [](const auto& check)
								   {
									   return check.state == "proved";
								   })
			? 0
			: 1;
	}

	[[nodiscard]] int run_missing(const int argc, char** argv)
	{
		std::string_view project_path;
		std::string_view use_case;
		std::string_view format = "json";
		bool project_seen{};
		bool use_case_seen{};
		bool format_seen{};
		for (int index = 2; index < argc; ++index)
		{
			const std::string_view argument{argv[index]};
			auto read_option = [&](std::string_view& target, const std::string_view field) -> bool
			{
				if (++index >= argc)
				{
					std::cerr << "doctor.cli-invalid field=" << field << " detail=missing-value\n";
					return false;
				}
				target = argv[index];
				return !target.empty();
			};
			if (argument == "--project")
			{
				if (project_seen)
				{
					std::cerr << "doctor.cli-invalid field=project detail=duplicate-option\n";
					return 2;
				}
				project_seen = true;
				if (!read_option(project_path, "project"))
					return 2;
				continue;
			}
			if (argument.rfind("--project=", 0U) == 0U)
			{
				if (project_seen)
				{
					std::cerr << "doctor.cli-invalid field=project detail=duplicate-option\n";
					return 2;
				}
				project_seen = true;
				project_path = argument.substr(10U);
				continue;
			}
			if (argument == "--use-case")
			{
				if (use_case_seen)
				{
					std::cerr << "doctor.cli-invalid field=use_case_id detail=duplicate-option\n";
					return 2;
				}
				use_case_seen = true;
				if (!read_option(use_case, "use_case_id"))
					return 2;
				continue;
			}
			if (argument.rfind("--use-case=", 0U) == 0U)
			{
				if (use_case_seen)
				{
					std::cerr << "doctor.cli-invalid field=use_case_id detail=duplicate-option\n";
					return 2;
				}
				use_case_seen = true;
				use_case = argument.substr(11U);
				continue;
			}
			if (argument == "--format")
			{
				if (format_seen)
				{
					std::cerr << "doctor.cli-invalid field=format detail=duplicate-option\n";
					return 2;
				}
				format_seen = true;
				if (++index >= argc)
				{
					std::cerr << "doctor.cli-invalid field=format detail=missing-value\n";
					return 2;
				}
				format = argv[index];
				continue;
			}
			if (argument.rfind("--format=", 0U) == 0U)
			{
				if (format_seen)
				{
					std::cerr << "doctor.cli-invalid field=format detail=duplicate-option\n";
					return 2;
				}
				format_seen = true;
				format = argument.substr(9U);
				continue;
			}
			std::cerr << "doctor.cli-invalid field=argument detail=unexpected-argument\n";
			return 2;
		}
		if (project_path.empty())
		{
			std::cerr << "doctor.project-required field=project detail=missing\n";
			return 2;
		}
		if (use_case.empty())
		{
			std::cerr << "doctor.use-case-required field=use_case_id detail=missing\n";
			return 2;
		}
		if (project_path.size() > cxxlens::sdk::doctor::maximum_json_string_bytes ||
			use_case.size() > cxxlens::sdk::doctor::maximum_json_string_bytes)
		{
			std::cerr << "doctor.cli-invalid field=argument detail=byte-limit\n";
			return 2;
		}
		if (format != "json" && format != "markdown")
		{
			std::cerr << "doctor.cli-invalid field=format detail=unsupported\n";
			return 2;
		}
		std::string read_error;
		const auto raw = cxxlens::sdk::doctor::read_file(project_path, read_error);
		if (!read_error.empty())
		{
			std::cerr << read_error << '\n';
			return 2;
		}
		const auto project = cxxlens::sdk::doctor::parse_project_document(raw);
		if (std::holds_alternative<product_error>(project))
		{
			print_error(std::get<product_error>(project));
			return 2;
		}
		const cxxlens::sdk::doctor::installed_product_catalog_loader catalog_loader;
		auto loaded_catalog = catalog_loader.load();
		if (std::holds_alternative<product_error>(loaded_catalog))
		{
			print_error(std::get<product_error>(loaded_catalog));
			return 2;
		}
		const cxxlens::sdk::doctor::installed_product_authority_loader authority_loader;
		const cxxlens::sdk::doctor::unavailable_signature_verifier signature_verifier;
		const cxxlens::sdk::doctor::unavailable_authority_state_port authority_state;
		const auto authority_source =
			cxxlens::sdk::doctor::installed_authority_source_from_paths(argv[0]);
		auto loaded_authority =
			authority_loader.load(authority_source, signature_verifier, authority_state);
		if (std::holds_alternative<product_error>(loaded_authority))
		{
			print_error(std::get<product_error>(loaded_authority));
			return 2;
		}
		const auto resolved = cxxlens::sdk::doctor::resolve(
			use_case,
			std::get<cxxlens::sdk::doctor::project_context>(project),
			std::get<cxxlens::sdk::doctor::authenticated_capability_catalog>(
				std::move(loaded_catalog)),
			std::get<cxxlens::sdk::doctor::installed_product_authority_verifier>(loaded_authority));
		if (std::holds_alternative<product_error>(resolved))
		{
			print_error(std::get<product_error>(resolved));
			return 2;
		}
		const auto& resolution = std::get<cxxlens::sdk::doctor::resolution>(resolved);
		const auto json = canonical_json(cxxlens::sdk::doctor::to_json(resolution));
		if (!print_document(format, json))
			return 2;
		return resolution.state == cxxlens::sdk::doctor::resolution_state::proved ? 0 : 1;
	}

} // namespace

int main(int argc, char** argv)
{
	if (argc < 2)
	{
		std::cerr << "usage: cxxlens-sdk-doctor relation-presence <relation-id>... [--format "
					 "json|markdown]\n"
				  << "       cxxlens-sdk-doctor missing --project <project.json> --use-case <id> "
					 "[--format json|markdown]\n";
		return 2;
	}
	const std::string_view command{argv[1]};
	if (command == "relation-presence")
		return run_relation_presence(argc, argv);
	if (command == "missing")
		return run_missing(argc, argv);
	std::cerr << "usage: cxxlens-sdk-doctor relation-presence|missing\n";
	return 2;
}
