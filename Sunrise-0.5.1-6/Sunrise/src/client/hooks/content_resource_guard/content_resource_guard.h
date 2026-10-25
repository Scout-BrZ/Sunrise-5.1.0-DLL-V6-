#pragma once

namespace sunrise::client::hooks::content_resource_guard {
/** Instala a protecao somente na imagem com assinatura unica conhecida. */
[[nodiscard]] bool install() noexcept;
/** Restaura o codigo; false impede descarregar a DLL enquanto o stub estiver em uso. */
[[nodiscard]] bool uninstall() noexcept;
} // namespace sunrise::client::hooks::content_resource_guard
