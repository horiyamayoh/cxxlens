#pragma once

/** @file cc_call_site.hpp @brief Generated static tags for cc.call_site v1. */

#include <cxxlens/sdk/query.hpp>

namespace cxxlens::cc::relations
{
	/** @brief Generated relation tag backed by descriptor `cc.call_site.v1`. */
	struct call_site
	{
		[[nodiscard]] static const sdk::relation_descriptor& descriptor()
		{
			static const sdk::relation_descriptor value = []
			{
				sdk::relation_descriptor output;
				output.id = "cc.call_site.v1";
				output.name = "cc.call_site";
				output.version = {1U, 0U, 0U};
				output.semantic_major = 1U;
				output.columns = {
					{"cc.call_site.v1.call",
					 "call",
					 {sdk::scalar_kind::typed_id, "cc_call_id", false},
					 true},
					{"cc.call_site.v1.compile_unit",
					 "compile_unit",
					 {sdk::scalar_kind::typed_id, "compile_unit_id", false},
					 true},
					{"cc.call_site.v1.caller",
					 "caller",
					 {sdk::scalar_kind::typed_id, "cc_entity_id", true},
					 false},
					{"cc.call_site.v1.kind",
					 "kind",
					 {sdk::scalar_kind::open_symbol, "cc.call-kind/1", false},
					 true},
					{"cc.call_site.v1.source",
					 "source",
					 {sdk::scalar_kind::typed_id, "source_span_id", false},
					 true},
					{"cc.call_site.v1.receiver_static_type",
					 "receiver_static_type",
					 {sdk::scalar_kind::typed_id, "cc_type_id", true},
					 false},
					{"cc.call_site.v1.ordinal",
					 "ordinal",
					 {sdk::scalar_kind::unsigned_integer, {}, false},
					 true},
				};
				output.descriptor_digest =
					sdk::semantic_digest("cxxlens.relation-descriptor.v1", output.canonical_form());
				return output;
			}();
			return value;
		}

		struct call
		{
			[[nodiscard]] static sdk::column_ref ref()
			{
				return {call_site::descriptor().id,
						"cc.call_site.v1.call",
						{sdk::scalar_kind::typed_id, "cc_call_id", false}};
			}
		};
		struct compile_unit
		{
			[[nodiscard]] static sdk::column_ref ref()
			{
				return {call_site::descriptor().id,
						"cc.call_site.v1.compile_unit",
						{sdk::scalar_kind::typed_id, "compile_unit_id", false}};
			}
		};
		struct caller
		{
			[[nodiscard]] static sdk::column_ref ref()
			{
				return {call_site::descriptor().id,
						"cc.call_site.v1.caller",
						{sdk::scalar_kind::typed_id, "cc_entity_id", true}};
			}
		};
		struct kind
		{
			[[nodiscard]] static sdk::column_ref ref()
			{
				return {call_site::descriptor().id,
						"cc.call_site.v1.kind",
						{sdk::scalar_kind::open_symbol, "cc.call-kind/1", false}};
			}
		};
		struct source
		{
			[[nodiscard]] static sdk::column_ref ref()
			{
				return {call_site::descriptor().id,
						"cc.call_site.v1.source",
						{sdk::scalar_kind::typed_id, "source_span_id", false}};
			}
		};
		struct receiver_static_type
		{
			[[nodiscard]] static sdk::column_ref ref()
			{
				return {call_site::descriptor().id,
						"cc.call_site.v1.receiver_static_type",
						{sdk::scalar_kind::typed_id, "cc_type_id", true}};
			}
		};
		struct ordinal
		{
			[[nodiscard]] static sdk::column_ref ref()
			{
				return {call_site::descriptor().id,
						"cc.call_site.v1.ordinal",
						{sdk::scalar_kind::unsigned_integer, {}, false}};
			}
		};
	};
} // namespace cxxlens::cc::relations
