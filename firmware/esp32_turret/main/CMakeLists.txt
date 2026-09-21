set(srcs
    "app_main.c"      # 入口：NVS + NimBLE 初始化、事件回调挂接、电机总线上电
    "pad_scan.c"      # M2a：广播扫描 + SCAN_RSP 解析
    "pad_link.c"      # M2b：GAP —— 扫描 / 连接 / 断线重扫
    "pad_gatt.c"      # M2b：GATT —— 服务链发现 + 订阅 notify
    "pad_input.c"     # M2b/M2c/M2e：notify 解析 + 覆盖队列 + 摇杆 → ω_ref → 差速逆解
    "motor_twai.c")   # M2d/M2e：TWAI + Emm 协议 + 差速逆运动学

# MINIMAL_BUILD 开着（见工程根 CMakeLists），所以必须自己列全依赖，
# 不能像 blecent 官方例程那样什么都不写。
#   bt               —— NimBLE（nimble_port / ble_hs / console / ble_svc_gap）
#   nvs_flash        —— app_main 里要初始化 NVS
#   esp_timer        —— pad_input 的速率统计、motor_twai 的收发超时
#   esp_driver_twai  —— CAN（motor_twai.c）
idf_component_register(SRCS "${srcs}"
                       INCLUDE_DIRS "."
                       REQUIRES bt nvs_flash esp_timer esp_driver_twai)
