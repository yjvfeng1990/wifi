#ifndef ROLE_CONTROL_H
#define ROLE_CONTROL_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ROLE_GPIO_TRIGGER    GPIO_NUM_4
#define ROLE_ACTIVE_SECONDS  60

typedef enum {
    ROLE_OFF = 0,
    ROLE_BROADCAST = 1,   // Slave: 发送广播
    ROLE_RECEIVE = 2,     // Master: 接收广播
} role_type_t;

typedef enum {
    ROLE_STATE_IDLE = 0,
    ROLE_STATE_ACTIVE,
} role_state_t;

void role_control_init(void);

// 角色配置 (NVS持久化)
void role_control_set_role(role_type_t role);
role_type_t role_control_get_role(void);

// 启动/停止角色动作
bool role_control_start(void);
void role_control_stop(void);

// 状态查询
role_state_t role_control_get_state(void);
int role_control_get_remaining(void);

// 活跃的peer数量 (用于LED)
int role_control_get_peer_count(void);

// 通知：收到数据时调用
void role_control_notify_data(void);

#ifdef __cplusplus
}
#endif

#endif
