TLS trust anchors for Equinox OS
================================
The file equinox_anchors.c is generated. Do not hand-edit.

How it was produced (host with Internet access):

  1. Pick roots from the system store (Debian: /etc/ssl/certs/
     ca-certificates.crt, i.e. the Mozilla root set).
  2. Keep only keys BearSSL 0.6 can verify on i386:
     RSA (any size) and ECDSA P-256. Drop P-384 / Ed25519 roots
     (e.g. ISRG Root X2, GTS Root R3) - the engine cannot hold
     their keys, so they are dead weight.
  3. cat selected roots into one PEM, then:

       cd third_party/bearssl && make            # host build
       ./build/brssl ta -q selected.pem > \
           anchors/equinox_anchors.c

  4. Re-add the header comment and the two export wrappers
     at the end (eq_tls_anchors / eq_tls_anchors_num).

Embedded set (9 roots): DigiCert Global Root CA,
DigiCert Global Root G2, ISRG Root X1, GTS Root R1,
Amazon Root CA 1, Amazon Root CA 3, USERTrust RSA,
AAA Certificate Services, GlobalSign Root CA.

License: the generated C file embeds public key material of
public root CAs and is produced by BearSSL's `brssl ta` tool
(MIT licensed, see ../LICENSE.txt).
