#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>

#include <cxxlens/configuration.hpp>

#include "../core/canonical_json.hpp"
#include "configuration_data.hpp"
#include "configuration_loader.hpp"
#include "configuration_spec.hpp"

namespace cxxlens
{
	namespace
	{
		using detail::configuration_data;
		using detail::configuration_origin;
		using detail::configuration_value;
		using detail::json::json_value;

		[[nodiscard]] error
		configuration_error(std::string code, std::string reason, std::string key = {})
		{
			error failure;
			failure.code.value = std::move(code);
			failure.message = "configuration resolution failed";
			failure.attributes.emplace("reason", std::move(reason));
			if (!key.empty())
				failure.attributes.emplace("key", std::move(key));
			return failure;
		}

		[[nodiscard]] unsigned rank(const configuration_layer layer) noexcept
		{
			return static_cast<unsigned>(layer);
		}

		void canonical_sort(std::vector<configuration_origin>& history)
		{
			std::ranges::sort(history,
							  [](const auto& left, const auto& right)
							  {
								  if (left.layer != right.layer)
									  return rank(left.layer) > rank(right.layer);
								  return left.source < right.source;
							  });
		}

		[[nodiscard]] json_value project_value(const configuration_value& value, const bool redact)
		{
			if (redact)
				return "[redacted]";
			return std::visit(
				[](const auto& item) -> json_value
				{
					using item_type = std::decay_t<decltype(item)>;
					if constexpr (std::is_same_v<item_type, std::vector<std::string>>)
					{
						json_value::array output;
						for (const auto& string : item)
							output.emplace_back(string);
						return output;
					}
					else
						return json_value{item};
				},
				value);
		}

		[[nodiscard]] std::shared_ptr<const configuration_data> built_in_data()
		{
			auto data = std::make_shared<configuration_data>();
			for (const auto& [key, spec] : detail::config::configuration_specs())
				data->values[key].push_back(
					{configuration_layer::built_in_default, spec.default_value, "built-in"});
			return data;
		}

		[[nodiscard]] result<std::shared_ptr<const configuration_data>>
		combine_loaded(const std::shared_ptr<const configuration_data>& loaded)
		{
			auto combined = std::make_shared<configuration_data>(*built_in_data());
			combined->profiles = loaded->profiles;
			for (const auto& [key, origins] : loaded->values)
			{
				auto& target = combined->values[key];
				target.insert(target.end(), origins.begin(), origins.end());
				canonical_sort(target);
			}
			return std::shared_ptr<const configuration_data>{std::move(combined)};
		}

		[[nodiscard]] configuration_layer next_overlay_layer(const configuration_data& base)
		{
			auto maximum = configuration_layer::built_in_default;
			for (const auto& [key, history] : base.values)
			{
				(void)key;
				if (!history.empty() && rank(history.front().layer) > rank(maximum))
					maximum = history.front().layer;
			}
			if (maximum == configuration_layer::built_in_default)
				return configuration_layer::config_default;
			if (maximum == configuration_layer::config_default ||
				maximum == configuration_layer::named_profile)
				return configuration_layer::cli;
			return configuration_layer::api_option;
		}
	} // namespace

	configuration::configuration(std::shared_ptr<const detail::configuration_data> data)
		: data_{std::move(data)}
	{
	}

	result<configuration> configuration::defaults()
	{
		configuration result{built_in_data()};
		if (auto status = result.validate(); !status)
			return std::move(status.error());
		return result;
	}

	result<configuration> configuration::load(const path& yaml_file)
	{
		detail::runtime::standard_filesystem_adapter filesystem;
		detail::config::standard_environment_adapter environment;
		auto loaded = detail::config::load_document(yaml_file, filesystem, environment);
		if (!loaded)
			return std::move(loaded.error());
		auto combined = combine_loaded(loaded.value());
		if (!combined)
			return std::move(combined.error());
		configuration result{std::move(combined.value())};
		if (auto status = result.validate(); !status)
			return std::move(status.error());
		return result;
	}

	result<configuration> configuration::load_nearest(const path& start)
	{
		detail::runtime::standard_filesystem_adapter filesystem;
		detail::config::standard_environment_adapter environment;
		auto loaded = detail::config::load_nearest_document(start, filesystem, environment);
		if (!loaded)
			return std::move(loaded.error());
		auto combined = combine_loaded(loaded.value());
		if (!combined)
			return std::move(combined.error());
		configuration result{std::move(combined.value())};
		if (auto validation = result.validate(); !validation)
			return std::move(validation.error());
		return result;
	}

	result<configuration> configuration::with_profile(const std::string_view name) const
	{
		if (!data_)
			return configuration_error("config.invalid-value", "empty-configuration");
		const auto profile = data_->profiles.find(name);
		if (profile == data_->profiles.end())
			return configuration_error(
				"config.profile-not-found", "profile-not-found", std::string{name});
		auto result = std::make_shared<detail::configuration_data>(*data_);
		for (const auto& [key, value] : profile->second)
		{
			auto& history = result->values[key];
			history.push_back(
				{configuration_layer::named_profile, value, "profile:" + std::string{name}});
			canonical_sort(history);
		}
		configuration output{std::shared_ptr<const detail::configuration_data>{std::move(result)}};
		if (auto status = output.validate(); !status)
			return std::move(status.error());
		return output;
	}

	result<configuration> configuration::overlay(const configuration& higher) const
	{
		if (!data_ || !higher.data_)
			return configuration_error("config.overlay-conflict", "empty-configuration");
		const auto layer = next_overlay_layer(*data_);
		auto merged = std::make_shared<detail::configuration_data>(*data_);
		for (const auto& [key, origins] : higher.data_->values)
		{
			if (origins.empty() || origins.front().layer == configuration_layer::built_in_default)
				continue;
			const auto& winning = origins.front();
			auto& target = merged->values[key];
			if (!target.empty() && target.front().layer == configuration_layer::api_option &&
				layer == configuration_layer::api_option && target.front().value != winning.value)
				return configuration_error(
					"config.overlay-conflict", "same-layer-different-value", key);
			target.push_back({layer, winning.value, "overlay:" + winning.source});
			canonical_sort(target);
		}
		configuration output{std::shared_ptr<const detail::configuration_data>{std::move(merged)}};
		if (auto status = output.validate(); !status)
			return configuration_error("config.overlay-conflict",
									   status.error().attributes.at("reason"));
		return output;
	}

	result<void> configuration::validate() const
	{
		if (!data_)
			return configuration_error("config.invalid-value", "empty-configuration");
		for (const auto& [key, spec] : detail::config::configuration_specs())
		{
			(void)spec;
			const auto found = data_->values.find(key);
			if (found == data_->values.end() || found->second.empty())
				return configuration_error("config.invalid-value", "required-key-missing", key);
		}
		for (const auto& [key, history] : data_->values)
		{
			if (!detail::config::configuration_specs().contains(key))
				return configuration_error("config.unknown-key", "unknown-key", key);
			unsigned previous = rank(configuration_layer::api_option) + 1U;
			for (const auto& origin : history)
			{
				if (rank(origin.layer) > previous)
					return configuration_error("config.invalid-value", "provenance-order", key);
				previous = rank(origin.layer);
				if (auto status = detail::config::validate_configuration_value(key, origin.value);
					!status)
					return std::move(status.error());
			}
		}
		return {};
	}

	std::string configuration::resolved_json() const
	{
		if (auto status = validate(); !status)
			return {};
		json_value::object values;
		json_value::object provenance;
		for (const auto& [key, history] : data_->values)
		{
			values.emplace_back(
				key, project_value(history.front().value, detail::config::is_secret_key(key)));
			provenance.emplace_back(
				key, std::string{detail::config::configuration_layer_name(history.front().layer)});
		}
		auto document = detail::json::envelope(
			{"cxxlens.config.resolved.v1"},
			{{"provenance", std::move(provenance)}, {"values", std::move(values)}});
		auto encoded = detail::json::write(document);
		return encoded ? std::move(encoded.value()) : std::string{};
	}

	std::string configuration::explain(const std::string_view key) const
	{
		if (auto status = validate(); !status)
			return {};
		const auto found = data_->values.find(key);
		if (found == data_->values.end() || found->second.empty())
			return {};
		const bool redact = detail::config::is_secret_key(key);
		const auto project_origin = [&](const configuration_origin& origin)
		{
			return json_value::object{
				{"layer", std::string{detail::config::configuration_layer_name(origin.layer)}},
				{"source", redact ? json_value{"[redacted]"} : json_value{origin.source}},
				{"value", project_value(origin.value, redact)},
			};
		};
		json_value::array shadowed;
		for (auto origin = std::next(found->second.begin()); origin != found->second.end();
			 ++origin)
			shadowed.emplace_back(project_origin(*origin));
		auto document = detail::json::envelope({"cxxlens.config.explain.v1"},
											   {{"key", std::string{key}},
												{"shadowed", std::move(shadowed)},
												{"winner", project_origin(found->second.front())}});
		auto encoded = detail::json::write(document);
		return encoded ? std::move(encoded.value()) : std::string{};
	}
} // namespace cxxlens
