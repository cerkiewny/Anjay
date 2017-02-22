#include <avsystem/commons/log.h>
#include <anjay/anjay.h>
#include <anjay/security.h>
#include <anjay/server.h>
#include <anjay/attr_storage.h>
#include <anjay/access_control.h>

#include <poll.h>

#include "test_object.h"

int main_loop(anjay_t *anjay) {
    while (true) {
        // Obtain all network data sources
        AVS_LIST(avs_net_abstract_socket_t *const) sockets =
                anjay_get_sockets(anjay);

        // Prepare to poll() on them
        size_t numsocks = AVS_LIST_SIZE(sockets);
        struct pollfd pollfds[numsocks];
        size_t i = 0;
        AVS_LIST(avs_net_abstract_socket_t *const) sock;
        AVS_LIST_FOREACH(sock, sockets) {
            pollfds[i].fd = *(const int *) avs_net_socket_get_system(*sock);
            pollfds[i].events = POLLIN;
            pollfds[i].revents = 0;
            ++i;
        }

        const int max_wait_time_ms = 1000;
        // Determine the expected time to the next job in milliseconds.
        // If there is no job we will wait till something arrives for
        // at most 1 second (i.e. max_wait_time_ms).
        int wait_ms =
                anjay_sched_calculate_wait_time_ms(anjay, max_wait_time_ms);

        // Wait for the events if necessary, and handle them.
        if (poll(pollfds, numsocks, wait_ms) > 0) {
            int socket_id = 0;
            AVS_LIST(avs_net_abstract_socket_t *const) socket = NULL;
            AVS_LIST_FOREACH(socket, sockets) {
                if (pollfds[socket_id].revents) {
                    if (anjay_serve(anjay, *socket)) {
                        avs_log(tutorial, ERROR, "anjay_serve failed");
                    }
                }
                ++socket_id;
            }
        }

        // Finally run the scheduler (ignoring it's return value, which
        // is the amount of tasks executed)
        (void) anjay_sched_run(anjay);
    }
    return 0;
}

int main(int argc, char *argv[]) {
    static const anjay_configuration_t CONFIG = {
        .endpoint_name = "urn:dev:os:anjay-tutorial",
        .in_buffer_size = 4000,
        .out_buffer_size = 4000
    };

    anjay_t *anjay = anjay_new(&CONFIG);
    if (!anjay) {
        avs_log(tutorial, ERROR, "Could not create Anjay object");
        return -1;
    }
    int result = 0;

    // Instantiate necessary objects
    const anjay_dm_object_def_t **security_obj = anjay_security_object_create();
    const anjay_dm_object_def_t **server_obj = anjay_server_object_create();
    const anjay_dm_object_def_t **test_obj = create_test_object();
    const anjay_dm_object_def_t *const *access_control_obj =
            anjay_access_control_object_new(anjay);

    anjay_attr_storage_t *attr_storage = anjay_attr_storage_new(anjay);

    // For some reason we were unable to instantiate objects.
    if (!security_obj || !server_obj || !test_obj || !access_control_obj
            || !attr_storage) {
        result = -1;
        goto cleanup;
    }

    // Register them within Anjay
    if (anjay_register_object(anjay, anjay_attr_storage_wrap_object(
                                             attr_storage, security_obj))
        || anjay_register_object(anjay, anjay_attr_storage_wrap_object(
                                                attr_storage, server_obj))
        || anjay_register_object(anjay, anjay_attr_storage_wrap_object(
                                                attr_storage, test_obj))
        || anjay_register_object(anjay, anjay_attr_storage_wrap_object(
                                                attr_storage, access_control_obj))) {
        result = -1;
        goto cleanup;
    }

    // LwM2M Server account with SSID = 1
    const anjay_security_instance_t security_instance1 = {
        .ssid = 1,
        .server_uri = "coap://127.0.0.1:5683",
        .security_mode = ANJAY_UDP_SECURITY_NOSEC
    };

    const anjay_server_instance_t server_instance1 = {
        .ssid = 1,
        .lifetime = 86400,
        .default_min_period = -1,
        .default_max_period = -1,
        .disable_timeout = -1,
        .binding = ANJAY_BINDING_U
    };

    // LwM2M Server account with SSID = 2
    const anjay_security_instance_t security_instance2 = {
        .ssid = 2,
        .server_uri = "coap://127.0.0.1:5693",
        .security_mode = ANJAY_UDP_SECURITY_NOSEC
    };

    const anjay_server_instance_t server_instance2 = {
        .ssid = 2,
        .lifetime = 86400,
        .default_min_period = -1,
        .default_max_period = -1,
        .disable_timeout = -1,
        .binding = ANJAY_BINDING_U
    };

    // Setup first LwM2M Server
    anjay_iid_t server_instance_iid1 = ANJAY_IID_INVALID;
    anjay_security_object_add_instance(security_obj, &security_instance1,
                                       &(anjay_iid_t) { ANJAY_IID_INVALID });
    anjay_server_object_add_instance(server_obj, &server_instance1,
                                     &server_instance_iid1);

    // Setup second LwM2M Server
    anjay_iid_t server_instance_iid2 = ANJAY_IID_INVALID;
    anjay_security_object_add_instance(security_obj, &security_instance2,
                                       &(anjay_iid_t) { ANJAY_IID_INVALID });
    anjay_server_object_add_instance(server_obj, &server_instance2,
                                     &server_instance_iid2);

    // Set LwM2M Create permission rights for SSID = 1, this will make SSID=1
    // an exclusive owner of the Test Object
    anjay_access_control_set_acl(access_control_obj, 1234, ANJAY_IID_INVALID, 1,
                                 ANJAY_ACCESS_MASK_CREATE);

    // Allow both LwM2M Servers to read their Server Instances
    anjay_access_control_set_acl(access_control_obj, 1, server_instance_iid1,
                                 server_instance1.ssid, ANJAY_ACCESS_MASK_READ);
    anjay_access_control_set_acl(access_control_obj, 1, server_instance_iid2,
                                 server_instance2.ssid, ANJAY_ACCESS_MASK_READ);

    result = main_loop(anjay);

cleanup:
    anjay_delete(anjay);
    anjay_security_object_delete(security_obj);
    anjay_server_object_delete(server_obj);
    delete_test_object(test_obj);
    anjay_access_control_object_delete(access_control_obj);
    anjay_attr_storage_delete(attr_storage);
    return result;
}
