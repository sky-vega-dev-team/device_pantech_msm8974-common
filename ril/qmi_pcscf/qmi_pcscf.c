/*
 * Copyright (C) 2026 The LineageOS Project
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * qmi_pcscf: P-CSCF settings of the modem data profiles and calls over
 * QMI WDS.
 *
 * The legacy RIL of msm8974 never reports the P-CSCF of a data call, and
 * the modem only gets one from the network when the profile asks for it in
 * the PCO. This sets that up and reads the P-CSCF of the running IMS call
 * from the modem, for the IMS stack to use. It goes through the vendor
 * QCCI library (over qmuxd); the messages are encoded by hand so that they
 * do not depend on the IDL version of the vendor libqmiservices.
 *
 * usage:
 *   qmi_pcscf list                    modem profiles (3GPP)
 *   qmi_pcscf get <index>             settings of a profile (raw TLVs)
 *   qmi_pcscf set-ims <index> <pco> [imcn]
 *                                     ask for the P-CSCF in the PCO and/or mark
 *                                     the PDN as IMS signalling (-1 keeps it)
 *   qmi_pcscf setup <pco> <imcn>      set them on the profiles of the ims APN,
 *                                     clear them on the other profiles
 *   qmi_pcscf runtime [port]          settings of the call on a data port
 *   qmi_pcscf pcscf <port>            P-CSCF of the call on a data port
 *   qmi_pcscf voice-domain <pref>     voice domain preference of the modem, written
 *                                     when it differs (exit 10)
 *   qmi_pcscf nas-ssp                 NAS system selection preference (raw TLVs)
 *   qmi_pcscf nas-set <tlv> <u32> [perm]  one TLV of it, until the next power cycle
 *   qmi_pcscf cache                   store the P-CSCF of the IMS call (IM CN
 *                                     flag or ims APN, IPv4 or IPv6) in
 *                                     persist.vendor.ims.pcscf
 */

#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/system_properties.h>

/* QCCI ABI */
typedef void *qmi_client_type;
typedef void *qmi_idl_service_object_type;
typedef int qmi_client_error_type;
typedef void (*qmi_client_ind_cb)(qmi_client_type, unsigned int, void *, unsigned int, void *);

#define QMI_CLIENT_INSTANCE_ANY 0xffff
/* the QMUX transport numbers the data ports (rmnet0...) from 128 */
#define QMI_QMUX_INSTANCE_BASE 128
#define QMI_DATA_PORTS 11
#define QMI_TIMEOUT_MS 5000

/* WDS */
#define WDS_MODIFY_PROFILE_SETTINGS 0x0028
#define WDS_GET_PROFILE_LIST 0x002A
#define WDS_GET_PROFILE_SETTINGS 0x002B
#define WDS_GET_RUNTIME_SETTINGS 0x002D
#define WDS_SET_CLIENT_IP_FAMILY_PREF 0x004D
#define TLV_IP_FAMILY_PREF 0x01
#define TLV_RT_APN_NAME 0x14

#define TLV_RESULT 0x02
#define TLV_PROFILE_ID 0x01
#define TLV_PROFILE_LIST 0x01
#define TLV_PROFILE_TYPE 0x10
#define PROFILE_TYPE_3GPP 0

/* profile settings, the same in GET and MODIFY */
#define TLV_PCSCF_USING_PCO 0x1F
#define TLV_IM_CN_FLAG 0x21
#define TLV_APN_NAME 0x14

/* GET_RUNTIME_SETTINGS */
#define TLV_REQUESTED_SETTINGS 0x10
#define RT_PROFILE_ID (1 << 0)
#define RT_APN_NAME (1 << 3)
#define RT_IP_ADDRESS (1 << 8)
#define RT_PCSCF_USING_PCO (1 << 10)
#define RT_PCSCF_SERVER_LIST (1 << 11)
#define RT_PCSCF_DOMAIN_LIST (1 << 12)
#define RT_IP_FAMILY (1 << 15)
#define RT_IM_CN_FLAG (1 << 16)
#define TLV_RT_IM_CN_FLAG 0x22
#define TLV_RT_PCSCF_V4_LIST 0x23
#define TLV_RT_PCSCF_V6_LIST 0x2E

#define PCSCF_PROP "persist.vendor.ims.pcscf"

static struct {
    void (*xport_register)(int instance);
    qmi_client_error_type (*init_instance)(qmi_idl_service_object_type, int, qmi_client_ind_cb,
                                           void *, void *, uint32_t, qmi_client_type *);
    qmi_client_error_type (*send_raw)(qmi_client_type, unsigned long, void *, int, void *, int,
                                      int *, int);
    qmi_client_error_type (*release)(qmi_client_type);
    qmi_idl_service_object_type (*wds_object)(int, int, int);
    qmi_idl_service_object_type (*nas_object)(int, int, int);
} q;

static qmi_idl_service_object_type wds;
static qmi_client_type client;
/* the QCCI OS parameters are a few pthread objects, leave room */
static uint8_t os_params[1024];

static int load(void) {
    void *cci = dlopen("libqmi_cci.so", RTLD_NOW);
    void *svc = dlopen("libqmiservices.so", RTLD_NOW);
    int minor;

    if (!cci || !svc) {
        fprintf(stderr, "dlopen: %s\n", dlerror());
        return -1;
    }
    q.xport_register = dlsym(cci, "qmi_cci_qmux_xport_register");
    q.init_instance = dlsym(cci, "qmi_client_init_instance");
    q.send_raw = dlsym(cci, "qmi_client_send_raw_msg_sync");
    q.release = dlsym(cci, "qmi_client_release");
    q.wds_object = dlsym(svc, "wds_get_service_object_internal_v01");
    q.nas_object = dlsym(svc, "nas_get_service_object_internal_v01");
    if (!q.xport_register || !q.init_instance || !q.send_raw || !q.release || !q.wds_object) {
        fprintf(stderr, "missing QCCI symbols\n");
        return -1;
    }

    /* any minor version the library knows is enough for raw messages */
    for (minor = 0; minor <= 0xff && !wds; minor++)
        wds = q.wds_object(0x01, minor, 0x06);
    if (!wds) {
        fprintf(stderr, "no WDS service object\n");
        return -1;
    }
    return 0;
}

static int connect_wds(int instance) {
    int rc;

    q.xport_register(instance);
    rc = q.init_instance(wds, instance, NULL, NULL, os_params, QMI_TIMEOUT_MS, &client);
    if (rc) {
        fprintf(stderr, "qmi_client_init_instance(%d): %d\n", instance, rc);
        return -1;
    }
    return 0;
}

static void disconnect_wds(void) {
    q.release(client);
    client = NULL;
}

/* TLV helpers */
static int put_tlv(uint8_t *buf, int off, uint8_t type, const void *val, uint16_t len) {
    buf[off] = type;
    buf[off + 1] = len & 0xff;
    buf[off + 2] = len >> 8;
    memcpy(buf + off + 3, val, len);
    return off + 3 + len;
}

static const uint8_t *find_tlv(const uint8_t *buf, int len, uint8_t type, uint16_t *tlen) {
    int off = 0;

    while (off + 3 <= len) {
        uint16_t l = buf[off + 1] | (buf[off + 2] << 8);

        if (off + 3 + l > len)
            break;
        if (buf[off] == type) {
            *tlen = l;
            return buf + off + 3;
        }
        off += 3 + l;
    }
    return NULL;
}

static void dump_tlvs(const uint8_t *buf, int len) {
    int off = 0, i;

    while (off + 3 <= len) {
        uint16_t l = buf[off + 1] | (buf[off + 2] << 8);

        if (off + 3 + l > len)
            break;
        printf("  tlv 0x%02x len %u:", buf[off], l);
        for (i = 0; i < l; i++)
            printf(" %02x", buf[off + 3 + i]);
        printf("\n");
        off += 3 + l;
    }
}

static int request(unsigned long msg, uint8_t *req, int req_len, uint8_t *resp, int *resp_len,
                   int quiet) {
    uint16_t tlen;
    const uint8_t *res;
    int rc;

    rc = q.send_raw(client, msg, req, req_len, resp, *resp_len, resp_len, QMI_TIMEOUT_MS);
    if (rc) {
        fprintf(stderr, "message 0x%04lx: transport error %d\n", msg, rc);
        return -1;
    }
    res = find_tlv(resp, *resp_len, TLV_RESULT, &tlen);
    if (!res || tlen < 4) {
        fprintf(stderr, "message 0x%04lx: no result\n", msg);
        return -1;
    }
    if (res[0] | (res[1] << 8)) {
        if (!quiet)
            fprintf(stderr, "message 0x%04lx: error 0x%04x\n", msg, res[2] | (res[3] << 8));
        return -1;
    }
    return 0;
}

static int cmd_list(void) {
    uint8_t req[16], resp[2048];
    uint8_t type = PROFILE_TYPE_3GPP;
    int len = sizeof(resp), n, i, off;
    const uint8_t *list;
    uint16_t tlen;

    if (request(WDS_GET_PROFILE_LIST, req, put_tlv(req, 0, TLV_PROFILE_TYPE, &type, 1), resp, &len,
                0))
        return 1;
    list = find_tlv(resp, len, TLV_PROFILE_LIST, &tlen);
    if (!list || !tlen)
        return 1;
    n = list[0];
    for (i = 0, off = 1; i < n && off + 3 <= tlen; i++) {
        int name_len = list[off + 2];

        if (off + 3 + name_len > tlen)
            break;
        printf("%u %.*s\n", list[off + 1], name_len, (const char *)list + off + 3);
        off += 3 + name_len;
    }
    return 0;
}

static int cmd_get(int index) {
    uint8_t req[16], resp[4096];
    uint8_t id[2] = {PROFILE_TYPE_3GPP, (uint8_t)index};
    int len = sizeof(resp);

    if (request(WDS_GET_PROFILE_SETTINGS, req, put_tlv(req, 0, TLV_PROFILE_ID, id, 2), resp, &len,
                0))
        return 1;
    dump_tlvs(resp, len);
    return 0;
}

/* pco / imcn: 0 or 1, -1 leaves the flag alone */
static int cmd_set_ims(int index, int pco, int imcn) {
    uint8_t req[32], resp[256];
    uint8_t id[2] = {PROFILE_TYPE_3GPP, (uint8_t)index};
    uint8_t pco_val = pco ? 1 : 0, imcn_val = imcn ? 1 : 0;
    int len = sizeof(resp), off;

    off = put_tlv(req, 0, TLV_PROFILE_ID, id, 2);
    if (pco >= 0)
        off = put_tlv(req, off, TLV_PCSCF_USING_PCO, &pco_val, 1);
    if (imcn >= 0)
        off = put_tlv(req, off, TLV_IM_CN_FLAG, &imcn_val, 1);
    if (request(WDS_MODIFY_PROFILE_SETTINGS, req, off, resp, &len, 0))
        return 1;
    printf("profile %d: P-CSCF in PCO %d, IM CN flag %d\n", index, pco, imcn);
    return 0;
}

/*
 * setup <pco> <imcn>: the profiles with the ims APN get the flags, the others get
 * none of them. LG U+ modems crash when another profile keeps the IM CN flag.
 */
static int cmd_setup(int pco, int imcn) {
    uint8_t req[16], resp[2048];
    uint8_t type = PROFILE_TYPE_3GPP;
    int len = sizeof(resp), n, i, off, rc = 0, found = 0;
    const uint8_t *list;
    uint16_t tlen;

    if (request(WDS_GET_PROFILE_LIST, req, put_tlv(req, 0, TLV_PROFILE_TYPE, &type, 1), resp, &len,
                0))
        return 1;
    list = find_tlv(resp, len, TLV_PROFILE_LIST, &tlen);
    if (!list || !tlen)
        return 1;
    n = list[0];
    for (i = 0, off = 1; i < n && off + 3 <= tlen; i++) {
        uint8_t preq[16], presp[4096];
        uint8_t id[2] = {PROFILE_TYPE_3GPP, list[off + 1]};
        int plen = sizeof(presp), ims;
        const uint8_t *apn;
        uint16_t alen;

        off += 3 + list[off + 2];
        if (request(WDS_GET_PROFILE_SETTINGS, preq, put_tlv(preq, 0, TLV_PROFILE_ID, id, 2),
                    presp, &plen, 0)) {
            rc = 1;
            continue;
        }
        apn = find_tlv(presp, plen, TLV_APN_NAME, &alen);
        ims = apn && alen == 3 && !strncasecmp((const char *)apn, "ims", 3);
        found |= ims;
        if (cmd_set_ims(id[1], ims ? pco : 0, ims ? imcn : 0))
            rc = 1;
    }
    if (!found)
        fprintf(stderr, "no profile with the ims APN\n");
    return rc;
}

static int runtime(uint8_t *resp, int *len, int quiet) {
    uint8_t req[16];
    uint32_t mask = RT_PROFILE_ID | RT_APN_NAME | RT_IP_ADDRESS | RT_PCSCF_USING_PCO |
                    RT_PCSCF_SERVER_LIST | RT_PCSCF_DOMAIN_LIST | RT_IP_FAMILY | RT_IM_CN_FLAG;

    return request(WDS_GET_RUNTIME_SETTINGS, req,
                   put_tlv(req, 0, TLV_REQUESTED_SETTINGS, &mask, 4), resp, len, quiet);
}

/*
 * A client only sees the call of its IP family, IPv4 by default. The IMS
 * call of an IPv6 only network (LG U+) needs an IPv6 client.
 */
static int runtime_any_family(uint8_t *resp, int *len, int quiet) {
    uint8_t req[8], r[64], family = 6;
    int rlen = sizeof(r), size = *len;

    if (!runtime(resp, len, 1))
        return 0;
    if (request(WDS_SET_CLIENT_IP_FAMILY_PREF, req,
                put_tlv(req, 0, TLV_IP_FAMILY_PREF, &family, 1), r, &rlen, quiet))
        return 1;
    *len = size;
    return runtime(resp, len, quiet);
}

static int cmd_runtime(int port) {
    uint8_t resp[4096];
    int len = sizeof(resp), rc;

    if (connect_wds(QMI_QMUX_INSTANCE_BASE + port))
        return 1;
    rc = runtime_any_family(resp, &len, 0);
    if (!rc)
        dump_tlvs(resp, len);
    disconnect_wds();
    return rc ? 1 : 0;
}

/* appends the P-CSCF addresses of a runtime settings response */
static int append_pcscf(const uint8_t *resp, int len, char *out, size_t size) {
    const uint8_t *v;
    uint16_t tlen;
    int n = 0, i, j;

    v = find_tlv(resp, len, TLV_RT_PCSCF_V4_LIST, &tlen);
    if (v && tlen >= 1 && tlen >= 1 + 4 * v[0]) {
        for (i = 0; i < v[0]; i++, n++) {
            const uint8_t *a = v + 1 + 4 * i;   /* little endian */

            snprintf(out + strlen(out), size - strlen(out), "%s%u.%u.%u.%u", *out ? " " : "",
                     a[3], a[2], a[1], a[0]);
        }
    }

    v = find_tlv(resp, len, TLV_RT_PCSCF_V6_LIST, &tlen);
    if (v && tlen >= 1 && tlen >= 1 + 16 * v[0]) {
        for (i = 0; i < v[0]; i++, n++) {
            const uint8_t *a = v + 1 + 16 * i;

            snprintf(out + strlen(out), size - strlen(out), "%s", *out ? " " : "");
            for (j = 0; j < 16; j += 2)
                snprintf(out + strlen(out), size - strlen(out), "%s%x", j ? ":" : "",
                         (a[j] << 8) | a[j + 1]);
        }
    }
    return n;
}

/* pcscf <port>: P-CSCF addresses of the call on a data port, space separated */
static int cmd_pcscf(int port) {
    char pcscf[PROP_VALUE_MAX] = "";
    uint8_t resp[4096];
    int len = sizeof(resp), found = 0;

    if (connect_wds(QMI_QMUX_INSTANCE_BASE + port))
        return 1;
    if (!runtime_any_family(resp, &len, 1))
        found = append_pcscf(resp, len, pcscf, sizeof(pcscf));
    disconnect_wds();
    if (!found)
        return 1;
    printf("%s\n", pcscf);
    return 0;
}

static int connect_nas(void) {
    qmi_idl_service_object_type nas = NULL;
    int minor, rc;

    if (!q.nas_object) {
        fprintf(stderr, "no NAS service object\n");
        return -1;
    }
    for (minor = 0; minor <= 0xff && !nas; minor++)
        nas = q.nas_object(0x01, minor, 0x06);
    if (!nas) {
        fprintf(stderr, "no NAS service object\n");
        return -1;
    }
    q.xport_register(QMI_CLIENT_INSTANCE_ANY);
    rc = q.init_instance(nas, QMI_CLIENT_INSTANCE_ANY, NULL, NULL, os_params, QMI_TIMEOUT_MS,
                         &client);
    if (rc) {
        fprintf(stderr, "qmi_client_init_instance(NAS): %d\n", rc);
        return -1;
    }
    return 0;
}

/*
 * nas-set <tlv> <u32> [perm]: one TLV of SET_SYSTEM_SELECTION_PREFERENCE, until the
 * next power cycle, or kept by the modem with perm
 */
static int cmd_nas_set(int tlv, uint32_t value, int permanent) {
    uint8_t req[32], resp[256];
    uint8_t duration = 0; /* power cycle, not permanent */
    int len = sizeof(resp), off, rc;

    if (connect_nas())
        return 1;
    off = permanent ? 0 : put_tlv(req, 0, 0x17, &duration, 1);
    off = put_tlv(req, off, (uint8_t)tlv, &value, 4);
    rc = request(0x0033, req, off, resp, &len, 0);
    q.release(client);
    client = NULL;
    printf("tlv 0x%02x = %u: %s\n", tlv, value, rc ? "failed" : "ok");
    return rc ? 1 : 0;
}

/*
 * voice-domain <pref>: voice domain preference the modem tells the network at
 * attach (0 CS only, 1 PS only, 2 CS preferred, 3 PS preferred). Only written when
 * it differs, it is kept by the modem. Exits with 10 when it was changed: the
 * network only learns it on the next attach.
 */
#define NAS_GET_SSP 0x0034
#define NAS_SET_SSP 0x0033
#define TLV_SSP_VOICE_DOMAIN_GET 0x20
#define TLV_SSP_VOICE_DOMAIN_SET 0x23

static int cmd_voice_domain(uint32_t pref) {
    uint8_t req[16], resp[2048];
    const uint8_t *v;
    uint16_t tlen;
    uint32_t cur;
    int len = sizeof(resp), rc = 1;

    if (connect_nas())
        return 1;
    if (request(NAS_GET_SSP, req, 0, resp, &len, 0))
        goto out;
    v = find_tlv(resp, len, TLV_SSP_VOICE_DOMAIN_GET, &tlen);
    if (!v || tlen != 4) {
        fprintf(stderr, "no voice domain preference\n");
        goto out;
    }
    cur = v[0] | (v[1] << 8) | (v[2] << 16) | ((uint32_t)v[3] << 24);
    if (cur == pref) {
        printf("voice domain preference %u unchanged\n", cur);
        rc = 0;
        goto out;
    }
    len = sizeof(resp);
    if (request(NAS_SET_SSP, req, put_tlv(req, 0, TLV_SSP_VOICE_DOMAIN_SET, &pref, 4), resp, &len,
                0))
        goto out;
    printf("voice domain preference %u -> %u\n", cur, pref);
    rc = 10;
out:
    q.release(client);
    client = NULL;
    return rc;
}

/* nas-ssp: raw TLVs of the NAS system selection preference (read only) */
static int cmd_nas_ssp(void) {
    qmi_idl_service_object_type nas = NULL;
    uint8_t resp[2048];
    int len = sizeof(resp), minor, rc;

    if (!q.nas_object) {
        fprintf(stderr, "no NAS service object\n");
        return 1;
    }
    for (minor = 0; minor <= 0xff && !nas; minor++)
        nas = q.nas_object(0x01, minor, 0x06);
    if (!nas) {
        fprintf(stderr, "no NAS service object\n");
        return 1;
    }
    q.xport_register(QMI_CLIENT_INSTANCE_ANY);
    rc = q.init_instance(nas, QMI_CLIENT_INSTANCE_ANY, NULL, NULL, os_params, QMI_TIMEOUT_MS,
                         &client);
    if (rc) {
        fprintf(stderr, "qmi_client_init_instance(NAS): %d\n", rc);
        return 1;
    }
    rc = request(0x0034, NULL, 0, resp, &len, 0);
    if (!rc)
        dump_tlvs(resp, len);
    q.release(client);
    client = NULL;
    return rc ? 1 : 0;
}

static int cmd_cache(void) {
    char pcscf[PROP_VALUE_MAX] = "";
    int port, found = 0;

    for (port = 0; port < QMI_DATA_PORTS; port++) {
        uint8_t resp[4096];
        const uint8_t *imcn;
        uint16_t tlen;
        int len = sizeof(resp);

        if (connect_wds(QMI_QMUX_INSTANCE_BASE + port))
            continue;
        /* the ports without a call answer out of call */
        if (!runtime_any_family(resp, &len, 1)) {
            const uint8_t *apn = find_tlv(resp, len, TLV_RT_APN_NAME, &tlen);
            /* the IMS call has the IM CN flag, or at least the ims APN */
            int ims = apn && tlen == 3 && !strncasecmp((const char *)apn, "ims", 3);

            imcn = find_tlv(resp, len, TLV_RT_IM_CN_FLAG, &tlen);
            if ((imcn && tlen == 1 && imcn[0]) || ims)
                found += append_pcscf(resp, len, pcscf, sizeof(pcscf));
        }
        disconnect_wds();
    }

    if (!found) {
        fprintf(stderr, "no P-CSCF on an IMS call\n");
        return 1;
    }
    printf("%s\n", pcscf);
    if (__system_property_set(PCSCF_PROP, pcscf)) {
        fprintf(stderr, "could not set %s\n", PCSCF_PROP);
        return 1;
    }
    return 0;
}

int main(int argc, char **argv) {
    int rc = 2;

    if (argc < 2) {
        fprintf(stderr, "usage: %s list|get <index>|set-ims <index> <pco> [imcn]|setup <pco> <imcn>|runtime [port]|pcscf <port>|voice-domain <pref>|nas-ssp|nas-set <tlv> <u32> [perm]|cache\n",
                argv[0]);
        return 2;
    }

    if (load())
        return 1;

    if (!strcmp(argv[1], "runtime"))
        return cmd_runtime(argc > 2 ? atoi(argv[2]) : 0);
    if (!strcmp(argv[1], "cache"))
        return cmd_cache();
    if (!strcmp(argv[1], "nas-ssp"))
        return cmd_nas_ssp();
    if (!strcmp(argv[1], "voice-domain") && argc > 2)
        return cmd_voice_domain((uint32_t)strtoul(argv[2], NULL, 0));
    if (!strcmp(argv[1], "nas-set") && argc > 3)
        return cmd_nas_set((int)strtol(argv[2], NULL, 0), (uint32_t)strtoul(argv[3], NULL, 0),
                           argc > 4 && !strcmp(argv[4], "perm"));
    if (!strcmp(argv[1], "pcscf") && argc > 2)
        return cmd_pcscf(atoi(argv[2]));

    if (connect_wds(QMI_CLIENT_INSTANCE_ANY))
        return 1;
    if (!strcmp(argv[1], "list"))
        rc = cmd_list();
    else if (!strcmp(argv[1], "get") && argc > 2)
        rc = cmd_get(atoi(argv[2]));
    else if (!strcmp(argv[1], "setup") && argc > 3)
        rc = cmd_setup(atoi(argv[2]), atoi(argv[3]));
    else if (!strcmp(argv[1], "set-ims") && argc > 3)
        /* set-ims <index> <0|1> sets both, set-ims <index> <pco> <imcn> each (-1 keeps it) */
        rc = cmd_set_ims(atoi(argv[2]), atoi(argv[3]), argc > 4 ? atoi(argv[4]) : atoi(argv[3]));
    disconnect_wds();
    return rc;
}
