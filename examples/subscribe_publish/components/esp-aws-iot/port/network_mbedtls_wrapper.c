/*
 * Copyright 2010-2015 Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * Additions Copyright 2016 Espressif Systems (Shanghai) PTE LTD
 *
 * Licensed under the Apache License, Version 2.0 (the "License").
 * You may not use this file except in compliance with the License.
 * A copy of the License is located at
 *
 *  http://aws.amazon.com/apache2.0
 *
 * or in the "license" file accompanying this file. This file is distributed
 * on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either
 * express or implied. See the License for the specific language governing
 * permissions and limitations under the License.
 */
#include <sys/param.h>
#include <stdbool.h>
#include <string.h>
#include "aws_iot_config.h"

#include <timer_platform.h>
#include <network_interface.h>

#include "aws_iot_config.h"
#include "aws_iot_error.h"
#include "network_interface.h"
#include "network_platform.h"

#include "mbedtls/esp_debug.h"

#ifdef CONFIG_AWS_IOT_USE_HARDWARE_SECURE_ELEMENT
#include "mbedtls/atca_mbedtls_wrap.h"
#include "tng_atca.h"
#include "tng_atcacert_client.h"
#endif

#include "esp_log.h"

static const char *TAG = "aws_iot";

/* This is the value used for ssl read timeout */
#ifndef IOT_SSL_READ_TIMEOUT_MS
	#define IOT_SSL_READ_TIMEOUT_MS 3
#endif

/* When this much time has elapsed after receiving MBEDTLS_ERR_SSL_WANT_READ
 * or MBEDTLS_ERR_SSL_WANT_WRITE, then iot_tls_write will return
 * NETWORK_SSL_WRITE_TIMEOUT_ERROR. */
#ifndef IOT_SSL_WRITE_RETRY_TIMEOUT_MS
	#define IOT_SSL_WRITE_RETRY_TIMEOUT_MS 10
#endif

/* When this much time has elapsed after receiving MBEDTLS_ERR_SSL_WANT_READ,
 * MBEDTLS_ERR_SSL_WANT_WRITE, or MBEDTLS_ERR_SSL_TIMEOUT, then iot_tls_read
 * will return NETWORK_SSL_READ_TIMEOUT_ERROR. */
#ifndef IOT_SSL_READ_RETRY_TIMEOUT_MS
	#define IOT_SSL_READ_RETRY_TIMEOUT_MS 10
#endif

/*
 * This is a function to do further verification if needed on the cert received.
 *
 * Currently used to print debug-level information about each cert.
 */
static int _iot_tls_verify_cert(void *data, mbedtls_x509_crt *crt, int depth, uint32_t *flags) {
    char buf[256];
    ((void) data);

    if (LOG_LOCAL_LEVEL >= ESP_LOG_DEBUG) {
        ESP_LOGD(TAG, "Verify requested for (Depth %d):", depth);
        mbedtls_x509_crt_info(buf, sizeof(buf) - 1, "", crt);
        ESP_LOGD(TAG, "%s", buf);

        if((*flags) == 0) {
            ESP_LOGD(TAG, "  This certificate has no flags");
        } else {
            ESP_LOGD(TAG, "Verify result:%s", buf);
        }
    }

    return 0;
}

static void _iot_tls_set_connect_params(Network *pNetwork, const char *pRootCALocation, const char *pDeviceCertLocation,
                                 const char *pDevicePrivateKeyLocation, const char *pDestinationURL,
                                 uint16_t destinationPort, uint32_t timeout_ms, bool ServerVerificationFlag) {
    pNetwork->tlsConnectParams.DestinationPort = destinationPort;
    pNetwork->tlsConnectParams.pDestinationURL = pDestinationURL;
    pNetwork->tlsConnectParams.pDeviceCertLocation = pDeviceCertLocation;
    pNetwork->tlsConnectParams.pDevicePrivateKeyLocation = pDevicePrivateKeyLocation;
    pNetwork->tlsConnectParams.pRootCALocation = pRootCALocation;
    pNetwork->tlsConnectParams.timeout_ms = timeout_ms;
    pNetwork->tlsConnectParams.ServerVerificationFlag = ServerVerificationFlag;
}

IoT_Error_t iot_tls_init(Network *pNetwork, const char *pRootCALocation, const char *pDeviceCertLocation,
                         const char *pDevicePrivateKeyLocation, const char *pDestinationURL,
                         uint16_t destinationPort, uint32_t timeout_ms, bool ServerVerificationFlag) {
    _iot_tls_set_connect_params(pNetwork, pRootCALocation, pDeviceCertLocation, pDevicePrivateKeyLocation,
                                pDestinationURL, destinationPort, timeout_ms, ServerVerificationFlag);

    pNetwork->connect = iot_tls_connect;
    pNetwork->read = iot_tls_read;
    pNetwork->write = iot_tls_write;
    pNetwork->disconnect = iot_tls_disconnect;
    pNetwork->isConnected = iot_tls_is_connected;
    pNetwork->destroy = iot_tls_destroy;

    pNetwork->tlsDataParams.flags = 0;

    return SUCCESS;
}

IoT_Error_t iot_tls_is_connected(Network *pNetwork) {
    /* Use this to add implementation which can check for physical layer disconnect */
    return NETWORK_PHYSICAL_LAYER_CONNECTED;
}

IoT_Error_t iot_tls_connect(Network *pNetwork, TLSConnectParams *params) {
    int ret = SUCCESS;
    TLSDataParams *tlsDataParams = NULL;
    char portBuffer[6];
    char info_buf[256];

    if(NULL == pNetwork) {
        return NULL_VALUE_ERROR;
    }

    if(NULL != params) {
        _iot_tls_set_connect_params(pNetwork, params->pRootCALocation, params->pDeviceCertLocation,
                                    params->pDevicePrivateKeyLocation, params->pDestinationURL,
                                    params->DestinationPort, params->timeout_ms, params->ServerVerificationFlag);
    }

    tlsDataParams = &(pNetwork->tlsDataParams);

    mbedtls_net_init(&(tlsDataParams->server_fd));
    mbedtls_ssl_init(&(tlsDataParams->ssl));
    mbedtls_ssl_config_init(&(tlsDataParams->conf));

#ifdef CONFIG_MBEDTLS_DEBUG
    mbedtls_esp_enable_debug_log(&(tlsDataParams->conf), 4);
#endif

    mbedtls_ctr_drbg_init(&(tlsDataParams->ctr_drbg));
    mbedtls_x509_crt_init(&(tlsDataParams->cacert));
    mbedtls_x509_crt_init(&(tlsDataParams->clicert));
    mbedtls_pk_init(&(tlsDataParams->pkey));

    ESP_LOGD(TAG, "Seeding the random number generator...");
    mbedtls_entropy_init(&(tlsDataParams->entropy));
    if((ret = mbedtls_ctr_drbg_seed(&(tlsDataParams->ctr_drbg), mbedtls_entropy_func, &(tlsDataParams->entropy),
                                    (const unsigned char *) TAG, strlen(TAG))) != 0) {
        ESP_LOGE(TAG, "failed! mbedtls_ctr_drbg_seed returned -0x%x", -ret);
        return NETWORK_MBEDTLS_ERR_CTR_DRBG_ENTROPY_SOURCE_FAILED;
    }

   /*  Load root CA...

       Certs/keys can be paths or they can be raw data. These use a
       very basic heuristic: if the cert starts with '/' then it's a
       path, if it's longer than this then it's raw cert data (PEM or DER,
       neither of which can start with a slash. */
    if (pNetwork->tlsConnectParams.pRootCALocation[0] == '/') {
        ESP_LOGD(TAG, "Loading CA root certificate from file ...");
        ret = mbedtls_x509_crt_parse_file(&(tlsDataParams->cacert), pNetwork->tlsConnectParams.pRootCALocation);
    } else {
        ESP_LOGD(TAG, "Loading embedded CA root certificate ...");
        ret = mbedtls_x509_crt_parse(&(tlsDataParams->cacert), (const unsigned char *)pNetwork->tlsConnectParams.pRootCALocation,
                                 strlen(pNetwork->tlsConnectParams.pRootCALocation)+1);
    }

    if(ret < 0) {
        ESP_LOGE(TAG, "failed!  mbedtls_x509_crt_parse returned -0x%x while parsing root cert", -ret);
        return NETWORK_X509_ROOT_CRT_PARSE_ERROR;
    }
    ESP_LOGD(TAG, "ok (%d skipped)", ret);

    /* Load client certificate... */
#ifdef CONFIG_AWS_IOT_USE_HARDWARE_SECURE_ELEMENT
    if (pNetwork->tlsConnectParams.pDeviceCertLocation[0] == '#') {
        const atcacert_def_t* cert_def = NULL;
        ESP_LOGD(TAG, "Using certificate stored in ATECC608A");
        ret = tng_get_device_cert_def(&cert_def);
        if (ret == 0) {
            ret = atca_mbedtls_cert_add(&(tlsDataParams->clicert), cert_def);
        } else {
            ESP_LOGE(TAG, "failed! could not load cert from ATECC608A, tng_get_device_cert_def returned %02x", ret);
        }
    } else
#endif
    if (pNetwork->tlsConnectParams.pDeviceCertLocation[0] == '/') {
        ESP_LOGD(TAG, "Loading client cert from file...");
        ret = mbedtls_x509_crt_parse_file(&(tlsDataParams->clicert),
                                          pNetwork->tlsConnectParams.pDeviceCertLocation);
    } else {
        ESP_LOGD(TAG, "Loading embedded client certificate...");
        ret = mbedtls_x509_crt_parse(&(tlsDataParams->clicert),
                                     (const unsigned char *)pNetwork->tlsConnectParams.pDeviceCertLocation,
                                     strlen(pNetwork->tlsConnectParams.pDeviceCertLocation)+1);
    }
    if(ret != 0) {
        ESP_LOGE(TAG, "failed!  mbedtls_x509_crt_parse returned -0x%x while parsing device cert", -ret);
        return NETWORK_X509_DEVICE_CRT_PARSE_ERROR;
    }

    /* Parse client private key... */
#ifdef CONFIG_AWS_IOT_USE_HARDWARE_SECURE_ELEMENT
    if (pNetwork->tlsConnectParams.pDevicePrivateKeyLocation[0] == '#') {
        int8_t slot_id = pNetwork->tlsConnectParams.pDevicePrivateKeyLocation[1] - '0';
        if (slot_id < 0 || slot_id > 9) {
            ESP_LOGE(TAG, "Invalid ATECC608A slot ID.");
            ret = NETWORK_PK_PRIVATE_KEY_PARSE_ERROR;
        } else {
            ESP_LOGD(TAG, "Using ATECC608A private key from slot %d", slot_id);
            ret = atca_mbedtls_pk_init(&(tlsDataParams->pkey), slot_id);
            if (ret != 0) {
                ESP_LOGE(TAG, "failed !  atca_mbedtls_pk_init returned %02x", ret);
            }
        }
    } else
#endif
    if (pNetwork->tlsConnectParams.pDevicePrivateKeyLocation[0] == '/') {
        ESP_LOGD(TAG, "Loading client private key from file...");
#ifdef MBEDTLS_2_X_COMPAT
        ret = mbedtls_pk_parse_keyfile(&(tlsDataParams->pkey),
                                       pNetwork->tlsConnectParams.pDevicePrivateKeyLocation,
                                       "");
#else
        ret = mbedtls_pk_parse_keyfile(&(tlsDataParams->pkey),
                                       pNetwork->tlsConnectParams.pDevicePrivateKeyLocation,
                                       "", mbedtls_ctr_drbg_random, NULL);
#endif
    } else {
        ESP_LOGD(TAG, "Loading embedded client private key...");
#ifdef MBEDTLS_2_X_COMPAT
        ret = mbedtls_pk_parse_key(&(tlsDataParams->pkey),
                                   (const unsigned char *)pNetwork->tlsConnectParams.pDevicePrivateKeyLocation,
                                   strlen(pNetwork->tlsConnectParams.pDevicePrivateKeyLocation)+1,
                                   (const unsigned char *)"", 0);
#else
        ret = mbedtls_pk_parse_key(&(tlsDataParams->pkey),
                                   (const unsigned char *)pNetwork->tlsConnectParams.pDevicePrivateKeyLocation,
                                   strlen(pNetwork->tlsConnectParams.pDevicePrivateKeyLocation)+1,
                                   (const unsigned char *)"", 0, mbedtls_ctr_drbg_random, NULL);
#endif
    }
    if(ret != 0) {
        ESP_LOGE(TAG, "failed!  mbedtls_pk_parse_key returned -0x%x while parsing private key", -ret);
        return NETWORK_PK_PRIVATE_KEY_PARSE_ERROR;
    }

    /* Done parsing certs */
    ESP_LOGD(TAG, "ok");
    snprintf(portBuffer, 6, "%d", pNetwork->tlsConnectParams.DestinationPort);
    ESP_LOGD(TAG, "Connecting to %s/%s...", pNetwork->tlsConnectParams.pDestinationURL, portBuffer);
    if((ret = mbedtls_net_connect(&(tlsDataParams->server_fd), pNetwork->tlsConnectParams.pDestinationURL,
                                  portBuffer, MBEDTLS_NET_PROTO_TCP)) != 0) {
        ESP_LOGE(TAG, "failed! mbedtls_net_connect returned -0x%x", -ret);
        switch(ret) {
            case MBEDTLS_ERR_NET_SOCKET_FAILED:
                return NETWORK_ERR_NET_SOCKET_FAILED;
            case MBEDTLS_ERR_NET_UNKNOWN_HOST:
                return NETWORK_ERR_NET_UNKNOWN_HOST;
            case MBEDTLS_ERR_NET_CONNECT_FAILED:
            default:
                return NETWORK_ERR_NET_CONNECT_FAILED;
        };
    }

    ret = mbedtls_net_set_block(&(tlsDataParams->server_fd));
    if(ret != 0) {
        ESP_LOGE(TAG, "failed! net_set_(non)block() returned -0x%x", -ret);
        return SSL_CONNECTION_ERROR;
    } ESP_LOGD(TAG, "ok");

    ESP_LOGD(TAG, "Setting up the SSL/TLS structure...");
    if((ret = mbedtls_ssl_config_defaults(&(tlsDataParams->conf), MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM,
                                          MBEDTLS_SSL_PRESET_DEFAULT)) != 0) {
        ESP_LOGE(TAG, "failed! mbedtls_ssl_config_defaults returned -0x%x", -ret);
        return SSL_CONNECTION_ERROR;
    }

    mbedtls_ssl_conf_verify(&(tlsDataParams->conf), _iot_tls_verify_cert, NULL);

    if(pNetwork->tlsConnectParams.ServerVerificationFlag == true) {
        mbedtls_ssl_conf_authmode(&(tlsDataParams->conf), MBEDTLS_SSL_VERIFY_REQUIRED);
    } else {
        mbedtls_ssl_conf_authmode(&(tlsDataParams->conf), MBEDTLS_SSL_VERIFY_OPTIONAL);
    }
    mbedtls_ssl_conf_rng(&(tlsDataParams->conf), mbedtls_ctr_drbg_random, &(tlsDataParams->ctr_drbg));

    mbedtls_ssl_conf_ca_chain(&(tlsDataParams->conf), &(tlsDataParams->cacert), NULL);
    ret = mbedtls_ssl_conf_own_cert(&(tlsDataParams->conf), &(tlsDataParams->clicert), &(tlsDataParams->pkey));
    if(ret != 0) {
        ESP_LOGE(TAG, "failed! mbedtls_ssl_conf_own_cert returned %d", ret);
        return SSL_CONNECTION_ERROR;
    }

    mbedtls_ssl_conf_read_timeout(&(tlsDataParams->conf), pNetwork->tlsConnectParams.timeout_ms);

#ifdef CONFIG_MBEDTLS_SSL_ALPN
    /* Use the AWS IoT ALPN extension for MQTT, if port 443 is requested */
    if (pNetwork->tlsConnectParams.DestinationPort == 443) {
        const char *alpnProtocols[] = { "x-amzn-mqtt-ca", NULL };
        if ((ret = mbedtls_ssl_conf_alpn_protocols(&(tlsDataParams->conf), alpnProtocols)) != 0) {
            ESP_LOGE(TAG, "failed! mbedtls_ssl_conf_alpn_protocols returned -0x%x", -ret);
            return SSL_CONNECTION_ERROR;
        }
    }
#endif

    if((ret = mbedtls_ssl_setup(&(tlsDataParams->ssl), &(tlsDataParams->conf))) != 0) {
        ESP_LOGE(TAG, "failed! mbedtls_ssl_setup returned -0x%x", -ret);
        return SSL_CONNECTION_ERROR;
    }
    if((ret = mbedtls_ssl_set_hostname(&(tlsDataParams->ssl), pNetwork->tlsConnectParams.pDestinationURL)) != 0) {
        ESP_LOGE(TAG, "failed! mbedtls_ssl_set_hostname returned %d", ret);
        return SSL_CONNECTION_ERROR;
    }
    ESP_LOGD(TAG, "SSL state connect : %d ", tlsDataParams->ssl.state);
    mbedtls_ssl_set_bio(&(tlsDataParams->ssl), &(tlsDataParams->server_fd), mbedtls_net_send, NULL,
                        mbedtls_net_recv_timeout);
    ESP_LOGD(TAG, "ok");

    ESP_LOGD(TAG, "SSL state connect : %d ", tlsDataParams->ssl.state);
    ESP_LOGD(TAG, "Performing the SSL/TLS handshake...");
    while((ret = mbedtls_ssl_handshake(&(tlsDataParams->ssl))) != 0) {
        if(ret != MBEDTLS_ERR_SSL_WANT_READ && ret != MBEDTLS_ERR_SSL_WANT_WRITE) {
            ESP_LOGE(TAG, "failed! mbedtls_ssl_handshake returned -0x%x", -ret);
            if(ret == MBEDTLS_ERR_X509_CERT_VERIFY_FAILED) {
                ESP_LOGE(TAG, "    Unable to verify the server's certificate. ");
            }
            return SSL_CONNECTION_ERROR;
        }
    }

    ESP_LOGD(TAG, "ok    [ Protocol is %s ]    [ Ciphersuite is %s ]", mbedtls_ssl_get_version(&(tlsDataParams->ssl)),
          mbedtls_ssl_get_ciphersuite(&(tlsDataParams->ssl)));
    if((ret = mbedtls_ssl_get_record_expansion(&(tlsDataParams->ssl))) >= 0) {
        ESP_LOGD(TAG, "    [ Record expansion is %d ]", ret);
    } else {
        ESP_LOGD(TAG, "    [ Record expansion is unknown (compression) ]");
    }

    ESP_LOGD(TAG, "Verifying peer X.509 certificate...");

    if(pNetwork->tlsConnectParams.ServerVerificationFlag == true) {
        if((tlsDataParams->flags = mbedtls_ssl_get_verify_result(&(tlsDataParams->ssl))) != 0) {
            ESP_LOGE(TAG, "failed");
            mbedtls_x509_crt_verify_info(info_buf, sizeof(info_buf), "  ! ", tlsDataParams->flags);
            ESP_LOGE(TAG, "%s", info_buf);
            ret = SSL_CONNECTION_ERROR;
        } else {
            ESP_LOGD(TAG, "ok");
            ret = SUCCESS;
        }
    } else {
        ESP_LOGW(TAG, " Server Verification skipped");
        ret = SUCCESS;
    }

    if(LOG_LOCAL_LEVEL >= ESP_LOG_DEBUG) {
        if (mbedtls_ssl_get_peer_cert(&(tlsDataParams->ssl)) != NULL) {
            ESP_LOGD(TAG, "Peer certificate information:");
            mbedtls_x509_crt_info((char *) info_buf, sizeof(info_buf) - 1, "      ", mbedtls_ssl_get_peer_cert(&(tlsDataParams->ssl)));
            ESP_LOGD(TAG, "%s", info_buf);
        }
    }

#ifdef CONFIG_AWS_IOT_SSL_SOCKET_NON_BLOCKING
	mbedtls_net_set_nonblock(&(tlsDataParams->server_fd));
#endif

    return (IoT_Error_t) ret;
}

IoT_Error_t iot_tls_write(Network *pNetwork, unsigned char *pMsg, size_t len, Timer *timer, size_t *written_len) {
	mbedtls_ssl_context *pSsl = &(pNetwork->tlsDataParams.ssl);
	size_t txLen = 0U;
	int ret = 0;
	/* This timer checks for a timeout whenever MBEDTLS_ERR_SSL_WANT_READ
	 * or MBEDTLS_ERR_SSL_WANT_WRITE are returned by mbedtls_ssl_write.
	 * Timeout is specified by IOT_SSL_WRITE_RETRY_TIMEOUT_MS. */
	Timer writeTimer;

	/* This variable is unused */
	(void) timer;

	/* The timer must be started in case no bytes are written on the first try */
	init_timer(&writeTimer);
	countdown_ms(&writeTimer, IOT_SSL_WRITE_RETRY_TIMEOUT_MS);

	while(len > 0U) {
		ret = mbedtls_ssl_write(pSsl, pMsg, len);

		if(ret > 0) {
			if((size_t) ret > len) {
				ESP_LOGE(TAG, "More bytes written than requested");
				return NETWORK_SSL_WRITE_ERROR;
			}

			/* Successfully sent data, so reset the timeout */
			init_timer(&writeTimer);
			countdown_ms(&writeTimer, IOT_SSL_WRITE_RETRY_TIMEOUT_MS);

			txLen += ret;
			pMsg += ret;
			len -= ret;
		} else if(ret == MBEDTLS_ERR_SSL_WANT_READ ||
				ret == MBEDTLS_ERR_SSL_WANT_WRITE) {
			if(has_timer_expired(&writeTimer)) {
				*written_len = txLen;
				return NETWORK_SSL_WRITE_TIMEOUT_ERROR;
			}
		} else {
			ESP_LOGE(TAG, " failed\n  ! mbedtls_ssl_write returned -0x%x", (unsigned int) -ret);
			/* All other negative return values indicate connection needs to be reset.
			 * Will be caught in ping request so ignored here */
			return NETWORK_SSL_WRITE_ERROR;
		}
	}

	*written_len = txLen;
	return SUCCESS;
}

IoT_Error_t iot_tls_read(Network *pNetwork, unsigned char *pMsg, size_t len, Timer *timer, size_t *read_len) {
    TLSDataParams *tlsDataParams = &(pNetwork->tlsDataParams);
	mbedtls_ssl_context *pSsl = &(pNetwork->tlsDataParams.ssl);
    mbedtls_ssl_config *ssl_conf = &(tlsDataParams->conf);
    uint32_t read_timeout;

	size_t rxLen = 0U;
	int ret;

    read_timeout = ssl_conf->read_timeout;

	/* This timer checks for a timeout whenever MBEDTLS_ERR_SSL_WANT_READ,
	 * MBEDTLS_ERR_SSL_WANT_WRITE, or MBEDTLS_ERR_SSL_TIMEOUT are returned by
	 * mbedtls_ssl_read. Timeout is specified by IOT_SSL_READ_RETRY_TIMEOUT_MS. */
	Timer readTimer;

	/* This variable is unused */
	(void) timer;

	/* The timer must be started in case no bytes are read on the first try */
	init_timer(&readTimer);
	countdown_ms(&readTimer, IOT_SSL_READ_RETRY_TIMEOUT_MS);

	while(len > 0U) {
        /* Make sure we never block on read for longer than timer has left,
         but also that we don't block indefinitely (ie read_timeout > 0) */
        mbedtls_ssl_conf_read_timeout(ssl_conf, MAX(1, MIN(read_timeout, left_ms(timer))));
		/* This read will timeout after IOT_SSL_READ_TIMEOUT_MS if there's no data to be read */
		ret = mbedtls_ssl_read(pSsl, pMsg, len);
        /* Restore the old timeout */
        mbedtls_ssl_conf_read_timeout(ssl_conf, read_timeout);

		if(ret > 0) {
			if((size_t) ret > len) {
				IOT_ERROR("More bytes read than requested\n\n");
				return NETWORK_SSL_WRITE_ERROR;
			}

			/* Successfully received data, so reset the timeout */
			init_timer(&readTimer);
			countdown_ms(&readTimer, IOT_SSL_READ_RETRY_TIMEOUT_MS);

			rxLen += ret;
			pMsg += ret;
			len -= ret;
		} else if(ret == MBEDTLS_ERR_SSL_WANT_READ || 
				ret == MBEDTLS_ERR_SSL_WANT_WRITE ||
				ret == MBEDTLS_ERR_SSL_TIMEOUT) {
			if(has_timer_expired(&readTimer)) {
				*read_len = rxLen;
				if(rxLen == 0U) {
					return NETWORK_SSL_NOTHING_TO_READ;
				} else {
					return NETWORK_SSL_READ_TIMEOUT_ERROR;
				}
			}
		} else {
			IOT_ERROR("Failed\n  ! mbedtls_ssl_read returned -0x%x\n\n", (unsigned int) -ret);
			return NETWORK_SSL_READ_ERROR;
		}
	}

	*read_len = rxLen;
	return SUCCESS;
}

IoT_Error_t iot_tls_disconnect(Network *pNetwork) {
    mbedtls_ssl_context *ssl = &(pNetwork->tlsDataParams.ssl);
    int ret = 0;
    do {
        ret = mbedtls_ssl_close_notify(ssl);
    } while(ret == MBEDTLS_ERR_SSL_WANT_WRITE);

    /* All other negative return values indicate connection needs to be reset.
     * No further action required since this is disconnect call */

    return SUCCESS;
}

IoT_Error_t iot_tls_destroy(Network *pNetwork) {
    TLSDataParams *tlsDataParams = &(pNetwork->tlsDataParams);

    mbedtls_net_free(&(tlsDataParams->server_fd));

    mbedtls_x509_crt_free(&(tlsDataParams->clicert));
    mbedtls_x509_crt_free(&(tlsDataParams->cacert));
    mbedtls_pk_free(&(tlsDataParams->pkey));
    mbedtls_ssl_free(&(tlsDataParams->ssl));
    mbedtls_ssl_config_free(&(tlsDataParams->conf));
    mbedtls_ctr_drbg_free(&(tlsDataParams->ctr_drbg));
    mbedtls_entropy_free(&(tlsDataParams->entropy));

    return SUCCESS;
}
