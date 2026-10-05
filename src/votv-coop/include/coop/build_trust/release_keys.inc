// The release public keys. One row per key:
//   MV_RELEASE_KEY(<id 1..254>, "<public key, 64 lowercase hex>", "<fixture signature, 128 lowercase hex>")
// The fixture signature is the key's own signature over SignedMessage("selftest-fixture", 0, 32 zero
// bytes) -- a message no main.dll can match -- so the boot selftest proves this row decodes and verifies.
// The rows are printed by the release signer's keygen, which also parses this file. An id is never
// reused; 255 is the selftest's test key and never appears here.
