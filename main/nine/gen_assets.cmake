# Build-time generation so a regenerated pack is always re-embedded.
configure_file("${TEMPLATE}" "${OUT}" @ONLY)
