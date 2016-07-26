#!/bin/sh

openssl ocsp \
	-no_nonce \
	-CAfile ca.pem \
	-verify_other demoCA/cacert.pem \
	-issuer demoCA/cacert.pem \
	-cert server.pem \
	-url http://localhost:8888/ \
	-respout ocsp-server-cache.der
