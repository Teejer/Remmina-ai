# Toggle KDE Output Device v2 support (default: ON)
option(ENABLE_KDE_OUTPUT_ORDER_V1 "Enable kde_output_order_v1 protocol support" ON)

if(ENABLE_KDE_OUTPUT_ORDER_V1)
    find_file(KDE_OUTPUT_ORDER_V1_XML
        NAMES "kde-output-order-v1.xml"
        PATHS "$ENV{XDG_DATA_DIRS}/plasma-wayland-protocols"
              "/usr/share/plasma-wayland-protocols"
              "/usr/local/share/plasma-wayland-protocols"
        NO_DEFAULT_PATH)

    if(KDE_OUTPUT_ORDER_V1_XML)
        message(STATUS "[OK] KDE Output Device v2 protocol found: ${KDE_OUTPUT_ORDER_V1_XML}")
        set(HAVE_KDE_OUTPUT_ORDER_V1 1)
		set(KDE_OUTPUT_ORDER_V1_XML_C "${CMAKE_BINARY_DIR}/generated/kde-output-order-v1-client.c")
        execute_process(
            COMMAND wayland-scanner client-header ${KDE_OUTPUT_ORDER_V1_XML} "generated/kde-output-order-v1-client.h"
			COMMAND wayland-scanner public-code ${KDE_OUTPUT_ORDER_V1_XML} "${KDE_OUTPUT_ORDER_V1_XML_C}"
            RESULT_VARIABLE SCANNER_RESULT
        )		
    else()
        message(STATUS "[SKIP] kde_output_order_v1 not found. Will fallback gracefully at runtime.")
		set(KDE_OUTPUT_ORDER_V1_XML_C "")
        set(HAVE_KDE_OUTPUT_ORDER_V1 0)
    endif()
	
endif()
set(KDE_OUTPKODE_GENERATED_C "${KDE_OUTPUT_ORDER_V1_XML_C}")

configure_file("${CMAKE_CURRENT_SOURCE_DIR}/config.h.in" "${CMAKE_BINARY_DIR}/config.h" @ONLY)