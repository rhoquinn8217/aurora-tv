#pragma once

/* The app's half of the pairing proof a bridge command carries: see
 * bridge_proof.c. */

/* Read this TV's paired key from key_dir (the first time; it is kept) and
 * hand the bridge a signer for the proof. With no key the bridge gets none,
 * and its commands go plainly, as before. Call before ctm_bridge_start(). */
void bridge_proof_register(const char *key_dir);
