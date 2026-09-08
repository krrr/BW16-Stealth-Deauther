#include <Arduino.h>
#include "api_all.h"
#include "http/HttpClient.h"
#include "OTA.h"

typedef uint32_t in_addr_t;

#ifdef __cplusplus
extern "C" {
#include "rtl8721d_ota.h"
#include <lwip/sockets.h>
}
#endif

void handleOtaApi(HttpClient& client) {
    int8_t sock_fd = client.getSock();
    if (sock_fd < 0) {
        client.sendJsonFail("Invalid socket descriptor");
        return;
    }

    // Set socket to blocking mode for low-level SDK OTA operations
    int flags = lwip_fcntl(sock_fd, F_GETFL, 0);
    lwip_fcntl(sock_fd, F_SETFL, flags & ~O_NONBLOCK);

    printf("[%s] Start OTA upload handler\n", __FUNCTION__);

    // Step 1: Allocate buffer
    unsigned char *alloc = NULL;
    int alloc_buf_size = BUF_SIZE; // 512
    uint32_t RevHdrLen = 0;
    update_ota_target_hdr OtaTargetHdr;
    uint32_t ota_target_index = OTA_INDEX_2;
    int ret = -1;

    alloc = (unsigned char *)ota_update_malloc(alloc_buf_size);
    if (!alloc) {
        printf("[%s] [ERROR] Alloc buffer failed\n", __FUNCTION__);
        client.sendJsonFail("Alloc buffer failed");
        return;
    }

    // Step 2: Determine target partition index
    OTA ota;
    if (ota.getOTACurAddr() == OTA_INDEX_1) {
        ota_target_index = OTA_INDEX_2;
        printf("[%s] Current index = 1, Target index = 2\n", __FUNCTION__);
    } else {
        ota_target_index = OTA_INDEX_1;
        printf("[%s] Current index = 2, Target index = 1\n", __FUNCTION__);
    }

    // Step 3: Receive firmware header
    // Since we parsed the HTTP headers, the socket read pointer is exactly at the start of the body.
    if (!recv_ota_file_hdr(alloc, &RevHdrLen, &OtaTargetHdr, sock_fd)) {
        printf("[%s] [ERROR] Receive firmware header failed\n", __FUNCTION__);
        client.sendJsonFail("Receive firmware header failed");
        goto ota_exit;
    }
    printf("[%s] Receive firmware header done, length: %d\n", __FUNCTION__, (int)RevHdrLen);

    // Debug: Print received header data to serial port
    printf("[%s] Received OTA header hex dump:\n", __FUNCTION__);
    for (uint32_t i = 0; i < RevHdrLen; i++) {
        printf("%02X ", alloc[i]);
        if ((i + 1) % 16 == 0) {
            printf("\n");
        }
    }
    if (RevHdrLen % 16 != 0) {
        printf("\n");
    }


    // Step 4: Parse target header
    if (!get_ota_tartget_header(alloc, RevHdrLen, &OtaTargetHdr, ota_target_index)) {
        printf("[%s] [ERROR] Get OTA target header failed\n", __FUNCTION__);
        client.sendJsonFail("Parse firmware header failed");
        goto ota_exit;
    }
    printf("[%s] Get OTA header done\n", __FUNCTION__);


    // Step 5: Erase flash space
    if (ota_target_index == OTA_INDEX_1) {
        printf("[%s] Erasing OTA target flash index 1 (0x%08X)...\n", __FUNCTION__, LS_IMG2_OTA1_ADDR);
        erase_ota_target_flash(LS_IMG2_OTA1_ADDR, OtaTargetHdr.FileImgHdr[0].ImgLen);
    } else {
        printf("[%s] Erasing OTA target flash index 2 (0x%08X)...\n", __FUNCTION__, LS_IMG2_OTA2_ADDR);
        erase_ota_target_flash(LS_IMG2_OTA2_ADDR, OtaTargetHdr.FileImgHdr[0].ImgLen);
    }
    printf("[%s] Erase flash done\n", __FUNCTION__);

    // Step 6: Download new firmware and write to flash
    // 固件文件头部有长度信息，库会自己判断。无需读取http的content-length
    printf("[%s] Downloading and writing firmware to flash...\n", __FUNCTION__);
    if (download_new_fw_from_server(sock_fd, &OtaTargetHdr, ota_target_index) == _FALSE) {
        printf("[%s] [ERROR] Download new firmware failed\n", __FUNCTION__);
        client.sendJsonFail("Download and write firmware failed");
        goto ota_exit;
    }
    printf("\n[%s] Download new firmware done\n", __FUNCTION__);

    // Step 7: Verify checksum and signature
    if (verify_ota_checksum(&OtaTargetHdr)) {
        if (!change_ota_signature(&OtaTargetHdr, ota_target_index)) {
            printf("[%s] [ERROR] Update OTA signature failed\n", __FUNCTION__);
            client.sendJsonFail("Update OTA signature failed");
            goto ota_exit;
        }
        printf("[%s] Update OTA signature done\n", __FUNCTION__);
        ret = 0;
    } else {
        printf("[%s] [ERROR] Verify OTA checksum failed\n", __FUNCTION__);
        client.sendJsonFail("Verify OTA checksum failed");
        goto ota_exit;
    }

    if (ret == 0) {
        printf("[%s] OTA upgrade succeeded! Sending response and rebooting...\n", __FUNCTION__);
        
        // Send success response
        JsonDocument doc;
        doc["success"] = true;
        doc["message"] = "OTA upgrade succeeded. Rebooting...";
        client.sendJson(doc);

        // Wait a bit to ensure the client receives the response before rebooting
        delay(1000);
        ota_platform_reset();
    }

ota_exit:
    if (alloc) {
        ota_update_free(alloc);
    }
}
