set(srcs
    "app_main.c"      # 入口：NVS + NimBLE 初始化、事件回调挂接
    "pad_scan.c"      # M2a：广播扫描 + SCAN_RSP 解析
    "pad_link.c"      # M2b：GAP —— 扫描 / 连接 / 断线重扫
    "pad_gatt.c"      # M2b：GATT —— 服务链发现 + 订阅 notify
    "pad_input.c")    # M2b/M2c：notify 解析 + 覆盖队列 + 摇杆 → ω_ref

idf_component_register(SRCS "${srcs}"
                       INCLUDE_DIRS ".")
