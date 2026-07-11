#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

#include <cxxlens/configuration.hpp>

#include "config/configuration_loader.hpp"
#include "config/environment_port.hpp"
#include "runtime/filesystem_port.hpp"

namespace
{
	class unreadable_filesystem final : public cxxlens::detail::runtime::filesystem_port
	{
	  public:
		[[nodiscard]] cxxlens::detail::runtime::runtime_result<std::string>
		read(const std::filesystem::path&,
			 const cxxlens::detail::runtime::request_context&) const override
		{
			using namespace cxxlens::detail::runtime;
			return unexpected{{runtime_status::permission_denied, "configuration.load", 13}};
		}

		[[nodiscard]] cxxlens::detail::runtime::runtime_result<std::vector<std::filesystem::path>>
		list(const std::filesystem::path&,
			 const cxxlens::detail::runtime::request_context&) const override
		{
			return std::vector<std::filesystem::path>{};
		}

		[[nodiscard]] cxxlens::detail::runtime::runtime_result<
			cxxlens::detail::runtime::file_status>
		stat(const std::filesystem::path&,
			 const cxxlens::detail::runtime::request_context&) const override
		{
			return cxxlens::detail::runtime::file_status{true, true, 1U};
		}

		[[nodiscard]] cxxlens::detail::runtime::runtime_result<std::filesystem::path>
		canonicalize(const std::filesystem::path& path,
					 const cxxlens::detail::runtime::request_context&) const override
		{
			return path.lexically_normal();
		}
	};

	[[nodiscard]] bool check(const bool condition, const std::string& message)
	{
		if (!condition)
			std::cerr << message << '\n';
		return condition;
	}

	void write_file(const std::filesystem::path& file, const std::string& content)
	{
		std::filesystem::create_directories(file.parent_path());
		std::ofstream output{file, std::ios::binary};
		output << content;
	}

	[[nodiscard]] bool has_error(const cxxlens::result<cxxlens::configuration>& result,
								 const std::string& code)
	{
		return !result && result.error().code.value == code;
	}

	[[nodiscard]] bool test_precedence_and_immutability(const std::filesystem::path& root)
	{
		const auto base_file = root / "base.yaml";
		const auto cli_file = root / "cli.yaml";
		const auto api_file = root / "api.yaml";
		write_file(base_file,
				   "schema: cxxlens.config.v1\n"
				   "execution:\n"
				   "  memory_budget_mb: 100\n"
				   "profiles:\n"
				   "  ci:\n"
				   "    execution: {memory_budget_mb: 200}\n");
		write_file(cli_file, "schema: cxxlens.config.v1\nexecution: {memory_budget_mb: 300}\n");
		write_file(api_file, "schema: cxxlens.config.v1\nexecution: {memory_budget_mb: 400}\n");

		auto base = cxxlens::configuration::load(base_file);
		auto cli = cxxlens::configuration::load(cli_file);
		auto api = cxxlens::configuration::load(api_file);
		bool passed = check(base && cli && api, "valid layered documents failed to load");
		if (!base || !cli || !api)
			return false;
		const auto immutable_before = base.value().resolved_json();
		auto profiled = base.value().with_profile("ci");
		passed &= check(profiled.has_value(), "named profile failed");
		if (!profiled)
			return false;
		auto with_cli = profiled.value().overlay(cli.value());
		passed &= check(with_cli.has_value(), "CLI overlay failed");
		if (!with_cli)
			return false;
		auto with_api = with_cli.value().overlay(api.value());
		passed &= check(with_api.has_value(), "API overlay failed");
		if (!with_api)
			return false;
		const auto explanation = with_api.value().explain("execution.memory_budget_mb");
		passed &= check(explanation.find("\"layer\":\"api_option\"") != std::string::npos,
						"API option did not win");
		const auto api_position = explanation.find("api_option");
		const auto cli_position = explanation.find("cli");
		const auto profile_position = explanation.find("named_profile");
		const auto config_position = explanation.find("config_default");
		const auto built_in_position = explanation.find("built_in_default");
		passed &=
			check(cli_position < profile_position && profile_position < config_position &&
					  config_position < built_in_position && api_position != std::string::npos,
				  "provenance order is unstable");
		passed &= check(explanation.find("\"value\":400") != std::string::npos,
						"resolved API value missing");
		passed &= check(base.value().resolved_json() == immutable_before,
						"overlay mutated its base input");
		auto conflict = with_api.value().overlay(cli.value());
		passed &= check(!conflict && conflict.error().code.value == "config.overlay-conflict",
						"same-layer overlay conflict was not reported");
		return passed;
	}

	[[nodiscard]] bool test_negative_and_redaction(const std::filesystem::path& root)
	{
		const auto unknown_file = root / "unknown.yaml";
		const auto type_file = root / "type.yaml";
		const auto semantic_environment_file = root / "semantic-env.yaml";
		const auto secret_file = root / "secret.yaml";
		write_file(unknown_file, "schema: cxxlens.config.v1\nanalysis: {macro_polciy: exclude}\n");
		write_file(type_file, "schema: cxxlens.config.v1\noutput: {deterministic: yes}\n");
		write_file(semantic_environment_file,
				   "schema: cxxlens.config.v1\nanalysis: {macro_policy: '${HOME}'}\n");
		write_file(secret_file, "schema: cxxlens.config.v1\nsecrets: {token: swordfish}\n");
		bool passed =
			check(has_error(cxxlens::configuration::load(unknown_file), "config.unknown-key"),
				  "unknown key did not use stable error");
		passed &= check(has_error(cxxlens::configuration::load(type_file), "config.invalid-value"),
						"wrong type did not use stable error");
		passed &= check(has_error(cxxlens::configuration::load(semantic_environment_file),
								  "config.invalid-value"),
						"semantic environment override was accepted");
		auto secret = cxxlens::configuration::load(secret_file);
		passed &= check(secret.has_value(), "secret configuration failed");
		if (secret)
		{
			const auto resolved = secret.value().resolved_json();
			const auto explained = secret.value().explain("secrets.token");
			passed &= check(resolved.find("swordfish") == std::string::npos &&
								explained.find("swordfish") == std::string::npos &&
								resolved.find("[redacted]") != std::string::npos,
							"secret leaked into support output");
			passed &= check(!secret.value().with_profile("missing") &&
								secret.value().with_profile("missing").error().code.value ==
									"config.profile-not-found",
							"missing profile did not use stable error");
		}
		return passed;
	}

	[[nodiscard]] bool test_limits_and_injected_filesystem()
	{
		using namespace cxxlens::detail;
		config::memory_environment_adapter environment;
		environment.set("PROJECT_ROOT", "project");
		runtime::memory_filesystem_adapter files;
		files
			.add("project/.cxxlens.yaml",
				 "schema: cxxlens.config.v1\n"
				 "workspace: {root: '${PROJECT_ROOT}'}\n"
				 "models:\n"
				 "  files:\n"
				 "    - project.yaml\n")
			.add("project/.cxxlens-root", "");
		auto loaded = config::load_document("project/.cxxlens.yaml", files, environment);
		bool passed = check(loaded.has_value(), "path interpolation through injected ports failed");
		runtime::memory_filesystem_adapter malformed;
		malformed.add("project/.cxxlens.yaml", "schema: [unterminated\n");
		auto invalid = config::load_document("project/.cxxlens.yaml", malformed, environment);
		passed &= check(!invalid && invalid.error().code.value == "config.yaml-invalid",
						"malformed YAML was accepted");
		for (const auto& unsafe :
			 {"schema: cxxlens.config.v1\noutput: {deterministic: true, deterministic: false}\n",
			  "schema: cxxlens.config.v1\nworkspace: {root: '*outside'}\n",
			  "schema: cxxlens.config.v1\nworkspace: {root: '!path outside'}\n"})
		{
			runtime::memory_filesystem_adapter unsafe_files;
			unsafe_files.add("project/.cxxlens.yaml", unsafe);
			auto unsafe_result =
				config::load_document("project/.cxxlens.yaml", unsafe_files, environment);
			passed &=
				check(!unsafe_result && unsafe_result.error().code.value == "config.yaml-invalid",
					  "duplicate/alias/tag YAML feature was accepted");
		}
		runtime::memory_filesystem_adapter huge;
		huge.add("project/.cxxlens.yaml", std::string(1024U * 1024U + 1U, 'x'));
		auto oversized = config::load_document("project/.cxxlens.yaml", huge, environment);
		passed &= check(!oversized && oversized.error().code.value == "config.yaml-invalid",
						"oversized YAML was accepted");
		std::string deep = "schema: cxxlens.config.v1\n";
		for (std::size_t index = 0U; index < 40U; ++index)
			deep += std::string(index * 2U, ' ') + "x:\n";
		runtime::memory_filesystem_adapter nested;
		nested.add("project/.cxxlens.yaml", deep);
		auto too_deep = config::load_document("project/.cxxlens.yaml", nested, environment);
		passed &= check(!too_deep && too_deep.error().code.value == "config.yaml-invalid",
						"deep YAML was accepted");
		unreadable_filesystem unreadable;
		auto unreadable_result =
			config::load_document("project/.cxxlens.yaml", unreadable, environment);
		passed &= check(!unreadable_result &&
							unreadable_result.error().code.value == "config.yaml-invalid",
						"unreadable YAML failure was not mapped structurally");
		return passed;
	}

	[[nodiscard]] bool test_nearest_boundary_and_symlink(const std::filesystem::path& root)
	{
		const auto project = root / "project";
		const auto child = project / "src" / "deep";
		write_file(project / ".cxxlens-root", "");
		write_file(project / ".cxxlens.yaml",
				   "schema: cxxlens.config.v1\noutput: {deterministic: true}\n");
		std::filesystem::create_directories(child);
		auto nearest = cxxlens::configuration::load_nearest(child);
		bool passed = check(nearest.has_value(), "nearest config was not found inside boundary");

		const auto outside = root / "outside.yaml";
		write_file(outside, "schema: cxxlens.config.v1\n");
		std::error_code error;
		std::filesystem::create_symlink(outside, child / ".cxxlens.yaml", error);
		if (!error)
		{
			auto escaped = cxxlens::configuration::load_nearest(child);
			passed &= check(!escaped && escaped.error().code.value == "config.yaml-invalid" &&
								escaped.error().attributes.at("reason") == "symlink-escape",
							"symlink escape was accepted");
		}
		std::filesystem::remove(child / ".cxxlens.yaml", error);
		std::filesystem::remove(project / ".cxxlens.yaml", error);
		write_file(root / ".cxxlens.yaml",
				   "schema: cxxlens.config.v1\noutput: {deterministic: false}\n");
		auto crossed = cxxlens::configuration::load_nearest(child);
		passed &= check(!crossed && crossed.error().code.value == "config.file-not-found",
						"nearest search crossed the project boundary");
		return passed;
	}
} // namespace

int main()
{
	const auto root = std::filesystem::temp_directory_path() / "cxxlens-configuration-unit";
	std::error_code ignored;
	std::filesystem::remove_all(root, ignored);
	std::filesystem::create_directories(root);
	const bool passed = test_precedence_and_immutability(root) &&
		test_negative_and_redaction(root) && test_limits_and_injected_filesystem() &&
		test_nearest_boundary_and_symlink(root);
	std::filesystem::remove_all(root, ignored);
	return passed ? 0 : 1;
}
