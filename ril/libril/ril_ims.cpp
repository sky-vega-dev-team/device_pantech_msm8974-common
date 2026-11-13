/*
 * Copyright (C) 2026 The LineageOS Project
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * IMS data calls of the Korean carriers on the legacy qcril.
 *
 * The modem only gets a P-CSCF from the network when the profile of the IMS
 * APN asks for it in the PCO, and qcril never reports it in the data call
 * response. Set up the profiles before an IMS data call, and fill the P-CSCF
 * of the calls from the modem, so that the IMS stack gets it from the link
 * properties. Both go through qmi_pcscf, in its own process, to stay away
 * from the QMI clients of qcril.
 */

#define LOG_TAG "RILC_IMS"

#include "ril_ims.h"

#include <arpa/inet.h>
#include <cutils/properties.h>
#include <log/log.h>
#include <strings.h>
#include <sys/wait.h>
#include <unistd.h>

#include <map>
#include <mutex>
#include <sstream>
#include <string>

namespace {

constexpr const char* kQmiPcscf = "/vendor/bin/qmi_pcscf";

std::mutex gLock;
// P-CSCF of the calls, by interface and addresses ("" when there is none)
std::map<std::string, std::string> gPcscf;

/*
 * Flags of the IMS APN profile: P-CSCF in the PCO, IM CN signalling flag.
 * The IM CN flag crashes the LG U+ modem (ps_utils.c:352).
 */
bool carrierFlags(int* pco, int* imcn) {
    char mccmnc[PROPERTY_VALUE_MAX];

    property_get("gsm.sim.operator.numeric", mccmnc, "");
    if (!strcmp(mccmnc, "45008") || !strcmp(mccmnc, "45005")) {  // KT, SKT
        *pco = 1;
        *imcn = 1;
        return true;
    }
    if (!strcmp(mccmnc, "45006")) {  // LG U+
        *pco = 1;
        *imcn = 0;
        return true;
    }
    return false;
}

// Only keeps the addresses: the vendor diag library prints on stdout too
std::string addressesOnly(const std::string& output) {
    std::istringstream in(output);
    std::string token, addresses;
    uint8_t buf[16];

    while (in >> token) {
        if (inet_pton(AF_INET, token.c_str(), buf) != 1 &&
            inet_pton(AF_INET6, token.c_str(), buf) != 1)
            continue;
        if (!addresses.empty()) addresses += " ";
        addresses += token;
    }
    return addresses;
}

// Runs qmi_pcscf, returns its exit status and output
int runQmiPcscf(const char* const argv[], std::string* out) {
    int pipefd[2];
    if (pipe(pipefd)) return -1;

    pid_t pid = fork();
    if (pid < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        return -1;
    }
    if (pid == 0) {
        dup2(pipefd[1], STDOUT_FILENO);
        close(pipefd[0]);
        close(pipefd[1]);
        execv(kQmiPcscf, const_cast<char* const*>(argv));
        _exit(127);
    }
    close(pipefd[1]);

    char buf[256];
    std::string output;
    ssize_t n;
    while ((n = read(pipefd[0], buf, sizeof(buf))) > 0) output.append(buf, n);
    close(pipefd[0]);

    int status;
    if (waitpid(pid, &status, 0) < 0 || !WIFEXITED(status)) return -1;
    if (out) *out = output;
    return WEXITSTATUS(status);
}

}  // namespace

void imsBeforeSetupDataCall(const char* apn) {
    int pco, imcn;

    if (!apn || strcasecmp(apn, "ims") || !carrierFlags(&pco, &imcn)) return;

    std::string pcoStr = std::to_string(pco), imcnStr = std::to_string(imcn);
    const char* const argv[] = {kQmiPcscf, "setup", pcoStr.c_str(), imcnStr.c_str(), nullptr};
    int rc = runQmiPcscf(argv, nullptr);
    RLOGI("IMS profiles set up (P-CSCF in PCO %d, IM CN %d): %d", pco, imcn, rc);
}

void imsFillPcscf(android::hardware::radio::V1_0::SetupDataCallResult& dcResult) {
    int pco, imcn;

    if (!dcResult.pcscf.empty() || dcResult.active == 0 || (int)dcResult.status != 0) return;
    if (strncmp(dcResult.ifname.c_str(), "rmnet", 5) || !carrierFlags(&pco, &imcn)) return;

    std::string key = std::string(dcResult.ifname) + " " + std::string(dcResult.addresses);
    std::lock_guard<std::mutex> lock(gLock);

    auto it = gPcscf.find(key);
    if (it == gPcscf.end()) {
        // rmnetN is the QMI data port N
        std::string port = dcResult.ifname.c_str() + 5;
        std::string pcscf;
        const char* const argv[] = {kQmiPcscf, "pcscf", port.c_str(), nullptr};
        if (runQmiPcscf(argv, &pcscf))
            pcscf.clear();
        else
            pcscf = addressesOnly(pcscf);
        RLOGI("P-CSCF of %s: [%s]", dcResult.ifname.c_str(), pcscf.c_str());
        it = gPcscf.emplace(key, pcscf).first;
    }
    if (!it->second.empty()) dcResult.pcscf = it->second;
}
