/**
 * @file captive_dns.h
 * @brief DNS server toi gian: tra loi TAT CA truy van DNS bang dia chi IP
 *        cua SoftAP (192.168.4.1). Nho vay khi dien thoai/laptop vua ket
 *        noi vao SoftAP, he dieu hanh se tu phat hien day la mang can dang
 *        nhap (captive portal) va tu mo trang cau hinh len.
 *
 * Day la tinh nang bo sung (nice-to-have). Neu khong dung module nay, nguoi
 * dung van co the truy cap bang cach tu mo trinh duyet va go http://192.168.4.1
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Khoi dong DNS server (chay trong 1 task nen rieng, port UDP 53). */
void captive_dns_start(void);

/** @brief Dung DNS server (xoa task va dong socket). */
void captive_dns_stop(void);

#ifdef __cplusplus
}
#endif