# Doxygen コメント規約

installed public header の callable は契約として文書化する。コメントは宣言側に一度だけ置き、実装側へ
複製しない。

callable の集合と exact signature は `schemas/cxxlens_ng_public_callable_inventory.yaml` が所有する。locked Clang 22 AST census と
Doxygen XML correspondence は独立した双方向 gate であり、header にだけある callable、inventory にだけある row、Doxygen にだけある
member、qualifier/default argument drift をいずれも拒否する。Doxygen の synthetic ID や表示用 signature を stable callable ID にしない。
Clang と Doxygen の column 座標値は異なるため、同一 header/line 内の declaration order で対応付けた両 source anchor を inventory に保存し、
各 anchor の qualified name と signature projection を照合する。global な件数や key 集合の一致だけを correspondence の証明にしない。

## 必須項目

- 全 callable: `@brief`, `@pre`, `@post`, `@note`, `@code{.cpp}`
- 実引数ごと: direction 付き `@param[in]`, `@param[out]`, `@param[in,out]`
- 非 `void` または結果分岐: `@retval`
- template parameter: `@tparam`
- 例外、危険性、非推奨、関連 API がある場合: `@throws`, `@warning`, `@deprecated`, `@see`

条件がない場合でも `@pre なし。` と明記する。`void` や引数なし API に架空の tag は追加しない。
`std::expected` は成功値と stable error code を別々の `@retval` で説明する。

## Free function

```cpp
/**
 * @brief 現在の API version 群を取得する。
 * @retval value library と schema の version 群。
 * @pre なし。
 * @post 外部状態を変更しない。
 * @note LLVM version は adapter の ABI ではなく設計 baseline を示す。
 * @code{.cpp}
 * #include <cxxlens/core.hpp>
 * int main() { return cxxlens::versions().library.major == 0U ? 0 : 1; }
 * @endcode
 */
[[nodiscard]] api_versions versions();
```

## Member function / accessor

```cpp
/**
 * @brief version を文字列へ変換する。
 * @retval value `major.minor.patch` と任意の prerelease からなる文字列。
 * @pre なし。
 * @post object と外部状態を変更しない。
 * @note prerelease が空の場合は suffix を付けない。
 * @code{.cpp}
 * #include <cxxlens/core.hpp>
 * int main() { return cxxlens::semantic_version{1U, 2U, 3U, {}}.to_string() == "1.2.3" ? 0 : 1; }
 * @endcode
 */
[[nodiscard]] std::string to_string() const;
```

## Constructor / template / expected

Constructor は構築後 invariant を `@post` に書く。template は型制約を `@tparam` と `@pre` に分ける。
`std::expected<T,error>` は `@retval value` と `@retval unexpected(error-code)` を列挙し、partial result、
coverage、unresolved、例外保証、thread safety を `@note` または専用節で説明する。

コード例は self-contained な `main` を含め、抽出後に Clang で syntax-check できる形にする。
