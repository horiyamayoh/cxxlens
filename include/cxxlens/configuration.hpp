#pragma once

/** @file configuration.hpp @brief Typed、immutable configuration resolver。 */

#include <memory>
#include <string>
#include <string_view>

#include <cxxlens/core/failure.hpp>
#include <cxxlens/source.hpp>

namespace cxxlens
{
	namespace detail
	{
		struct configuration_data;
	} // namespace detail

	/** @brief Configuration layer precedence。値が大きい layer が優先される。 */
	enum class configuration_layer : unsigned char
	{
		built_in_default,
		config_default,
		named_profile,
		cli,
		api_option,
	};

	/** @brief 検証済み設定と key ごとの provenance を保持する immutable value。 */
	class configuration
	{
	  public:
		/** @brief Safe built-in defaults を構築する。
		 * @retval value 検証済み defaults、または structured failure。
		 * @pre なし。 @post 成功値は immutable で `validate()` が成功する。
		 * @note Environment は参照しない。
		 * @code{.cpp}
		 * #include <cxxlens/configuration.hpp>
		 * int main(){auto c=cxxlens::configuration::defaults();return c?0:1;}
		 * @endcode */
		[[nodiscard]] static result<configuration> defaults();

		/** @brief 制限付き YAML document を読み込む。
		 * @param[in] yaml_file `.cxxlens.yaml` path。
		 * @retval value Config defaults と profiles、または stable config error。
		 * @pre Path は untrusted input として扱われる。 @post Unknown key は保持せず拒否する。
		 * @note Path placeholder 以外の environment interpolation は拒否する。
		 * @code{.cpp}
		 * #include <cxxlens/configuration.hpp>
		 * int main(){auto c=cxxlens::configuration::load(".cxxlens.yaml");return c?0:1;}
		 * @endcode */
		[[nodiscard]] static result<configuration> load(const path& yaml_file);

		/** @brief Start から最寄りの `.cxxlens.yaml` を project boundary 内で探す。
		 * @param[in] start File または directory path。
		 * @retval value 最寄りの configuration、または stable config error。
		 * @pre Start は存在する project 内 path。 @post `.git`/`.cxxlens-root` 境界を越えない。
		 * @note Canonical path containment により symlink escape を拒否する。
		 * @code{.cpp}
		 * #include <cxxlens/configuration.hpp>
		 * int main(){auto c=cxxlens::configuration::load_nearest(".");return c?0:1;}
		 * @endcode */
		[[nodiscard]] static result<configuration> load_nearest(const path& start);

		/** @brief Named profile を config defaults より高い layer として選択する。
		 * @param[in] name Profile name。
		 * @retval value 新しい configuration、または `config.profile-not-found`。
		 * @pre Current object は有効。 @post Input object は変更しない。
		 * @note Profile entry order は結果へ影響しない。
		 * @code{.cpp}
		 * #include <cxxlens/configuration.hpp>
		 * int main(){auto c=cxxlens::configuration::defaults();return
		 * c&&c.value().with_profile("ci")?0:1;}
		 * @endcode */
		[[nodiscard]] result<configuration> with_profile(std::string_view name) const;

		/** @brief Higher-precedence configuration を key ごとに immutable merge する。
		 * @param[in] higher Higher layer values。
		 * @retval value 新しい configuration、または `config.overlay-conflict`。
		 * @pre 両 input は有効。 @post 両 input を変更せず provenance を保持する。
		 * @note 同一 layer の異値は conflict として拒否する。
		 * @code{.cpp}
		 * #include <cxxlens/configuration.hpp>
		 * int main(){auto a=cxxlens::configuration::defaults();return
		 * a&&a.value().overlay(a.value())?0:1;}
		 * @endcode */
		[[nodiscard]] result<configuration> overlay(const configuration& higher) const;

		/** @brief 全 key の型、値、provenance invariant を検証する。
		 * @retval value Valid なら success、otherwise `config.invalid-value`。
		 * @pre なし。 @post Object を変更しない。
		 * @note Validation は diagnostic prose に依存しない。
		 * @code{.cpp}
		 * #include <cxxlens/configuration.hpp>
		 * int main(){auto c=cxxlens::configuration::defaults();return c&&c.value().validate()?0:1;}
		 * @endcode */
		[[nodiscard]] result<void> validate() const;

		/** @brief Redaction 済み versioned canonical JSON を返す。
		 * @retval value Key と winning provenance の canonical JSON。
		 * @pre `validate()` が成功する。 @post Object を変更しない。
		 * @note Secret value と operational metadata は出力しない。
		 * @code{.cpp}
		 * #include <cxxlens/configuration.hpp>
		 * int main(){auto c=cxxlens::configuration::defaults();return
		 * c&&c.value().resolved_json().empty()?1:0;}
		 * @endcode */
		[[nodiscard]] std::string resolved_json() const;

		/** @brief Key の winning layer と shadowed layers を安定順で説明する。
		 * @param[in] key Dotted configuration key。
		 * @retval value Redaction-safe canonical explanation JSON。Unknown key は空文字列。
		 * @pre `validate()` が成功する。 @post Object を変更しない。
		 * @note Secret-like value は常に `[redacted]`。
		 * @code{.cpp}
		 * #include <cxxlens/configuration.hpp>
		 * int main(){auto c=cxxlens::configuration::defaults();return
		 * c&&c.value().explain("output.deterministic").empty()?1:0;}
		 * @endcode */
		[[nodiscard]] std::string explain(std::string_view key) const;

	  private:
		explicit configuration(std::shared_ptr<const detail::configuration_data> data);
		std::shared_ptr<const detail::configuration_data> data_;
	};
} // namespace cxxlens
