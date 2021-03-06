#include <errno.h>
#include <stdio.h>
#include <unistd.h>

#include "bps.h"
#include "ips.h"
#include "log.h"
#include "ui.h"

static const char* PATCH_NEXT_MESSAGE = "Patching. Wrote %d hunks";
static const char* PATCH_SUCCESS_MESSAGE = "Success! Wrote %d hunks";
static const char* PATCH_FAIL_INVALID_OUTPUT_SIZE_MESSAGE = "ERR: Invalid output size!";
static const char* PATCH_FAIL_INVALID_OUTPUT_CHECKSUM_MESSAGE = "ERR: Invalid output checksum!";
static const char* PATCH_FAIL_ERR_IO = "ERR: Failed to open file!";
static const char* PATCH_FAIL_START = "ERR: Failed to start!";
static const char* PATCH_FAIL_UNKNOWN_TYPE = "ERR: Unknown patch type!";
static const char* PATCH_UNKNOWN_ERROR_MESSAGE = "ERR: Unknown end error!";

static const int DEFAULT_SLEEP = 16;

static void close_files(FILE* input_file, FILE* output_file, FILE* ips_file) {
    if (input_file != NULL) {
        fclose(input_file);
    }
    if (output_file != NULL) {
        fclose(output_file);
    }
    if (ips_file != NULL) {
        fclose(ips_file);
    }
}

static rombp_patch_type detect_patch_type(FILE* patch_file) {
    rombp_log_info("Trying to detect patch type\n");
    int rc = ips_verify_marker(patch_file);

    if (rc == 0) {
        rombp_log_info("Detected patch type: IPS\n");
        return PATCH_TYPE_IPS;
    }

    rc = fseek(patch_file, 0, SEEK_SET);
    if (rc == -1) {
        rombp_log_err("Error seeking patch file to beginning\n");
        return PATCH_TYPE_UNKNOWN;
    }

    rombp_log_info("Trying to detect BPS patch type\n");
    rc = bps_verify_marker(patch_file);
    if (rc == 0) {
        rombp_log_info("Detected patch type: BPS\n");
        return PATCH_TYPE_BPS;
    }

    return PATCH_TYPE_UNKNOWN;
}

// Used for any patch type specific data types that
// need to be passed into our start function.
typedef union {
    bps_file_header bps_file_header;
} rombp_patch_context;

static int start_patch(rombp_patch_type patch_type, rombp_patch_context* ctx, FILE* input_file, FILE* patch_file, FILE* output_file) {
    int rc;

    rombp_log_info("Start patching\n");

    switch (patch_type) {
        case PATCH_TYPE_IPS:
            rombp_log_info("Patch type started with IPS!\n");
            rc = ips_start(input_file, output_file);
            if (rc != PATCH_OK) {
                rombp_log_err("Failed to start patching IPS file: %d\n", rc);
                return -1;
            }
            return 0;
        case PATCH_TYPE_BPS:
            rc = bps_start(patch_file, &ctx->bps_file_header);
            if (rc != PATCH_OK) {
                rombp_log_err("Failed to start patching BPS file: %d\n", rc);
                return -1;
            }
            return 0;
        default:
            rombp_log_err("Cannot start unknown patch type\n");
            return -1;
    }
}

static rombp_patch_err end_patch(rombp_patch_type patch_type, rombp_patch_context* ctx, FILE* patch_file) {
    rombp_log_info("End patching\n");
    switch (patch_type) {
        case PATCH_TYPE_BPS: return bps_end(&ctx->bps_file_header, patch_file);
        case PATCH_TYPE_IPS:
        default:
            return PATCH_OK; // No cleanup work for IPS patches, by default nothing left to do.
    }
}

static rombp_hunk_iter_status next_hunk(rombp_patch_type patch_type, rombp_patch_context* patch_ctx, FILE* input_file, FILE* output_file, FILE* patch_file) {
    switch (patch_type) {
        case PATCH_TYPE_IPS: return ips_next(input_file, output_file, patch_file);
        case PATCH_TYPE_BPS: return bps_next(&patch_ctx->bps_file_header, input_file, output_file, patch_file);
        default: return HUNK_NONE;
    }
}

static rombp_patch_err open_patch_files(FILE** input_file, FILE** output_file, FILE** ips_file, rombp_patch_command* command) {
    *input_file = fopen(command->input_file, "r");
    if (*input_file == NULL) {
        rombp_log_err("Failed to open input file: %s, errno: %d\n", command->input_file, errno);
        return PATCH_ERR_IO;
    }

    *output_file = fopen(command->output_file, "w+");
    if (*output_file == NULL) {
        rombp_log_err("Failed to open output file: %d\n", errno);
        close_files(*input_file, NULL, NULL);
        return PATCH_ERR_IO;
    }

    *ips_file = fopen(command->ips_file, "r");
    if (*ips_file == NULL) {
        rombp_log_err("Failed to open IPS file: %d\n", errno);
        close_files(*input_file, *output_file, NULL);
        return PATCH_ERR_IO;
    }

    return PATCH_OK;
}

static void display_help() {
    fprintf(stderr, "rombp: IPS and BPS patcher\n\n");
    fprintf(stderr, "Usage:\n");
    fprintf(stderr, "rombp [options]\n\n");
    fprintf(stderr, "Options:\n");
    fprintf(stderr, "\t-i [FILE], Input ROM file\n");
    fprintf(stderr, "\t-p [FILE], IPS or BPS patch file\n");
    fprintf(stderr, "\t-o [FILE], Patched output file\n\n");
    fprintf(stderr, "Running rombp with no option arguments launches the SDL UI\n");
}

static int parse_command_line(int argc, char** argv, rombp_patch_command* command) {
    int c;

    while ((c = getopt(argc, argv, "i:p:o:")) != -1) {
        switch (c) {
            case 'i':
                command->input_file = optarg;
                break;
            case 'p':
                command->ips_file = optarg;
                break;
            case 'o':
                command->output_file = optarg;
                break;
            case '?':
                display_help();
                return -1;
            default:
                display_help();
                return -1;
        }
    }

    rombp_log_info("rombp arguments. input: %s, patch: %s, output: %s\n",
                   command->input_file, command->ips_file, command->output_file);

    return 0;
}

static void rombp_update_patch_status(rombp_patch_status* shared, rombp_patch_status* local) {
    if (shared != NULL && local != NULL) {
        int rc = pthread_mutex_lock(&shared->lock);
        if (rc != 0) {
            rombp_log_err("Failed to lock status mutex: %d\n", rc);
            exit(-1);
        }
        patch_status_copy(shared, local);
        rc = pthread_mutex_unlock(&shared->lock);
        if (rc != 0) {
            rombp_log_err("Failed to unlock mutex: %d\n", rc);
            exit(-1);
        }
    }
}

static void rombp_read_patch_status(rombp_patch_status* shared, rombp_patch_status* local) {
    if (shared != NULL && local != NULL) {
        int rc = pthread_mutex_lock(&shared->lock);
        if (rc != 0) {
            rombp_log_err("Failed to lock status mutex: %d\n", rc);
            exit(-1);
        }
        patch_status_copy(local, shared);
        rc = pthread_mutex_unlock(&shared->lock);
        if (rc != 0) {
            rombp_log_err("Failed to unlock mutex: %d\n", rc);
            exit(-1);
        }
    }
}

static int execute_patch(rombp_patch_command* command, rombp_patch_status* status) {
    int rc;
    rombp_patch_type patch_type = PATCH_TYPE_UNKNOWN;
    rombp_patch_context patch_ctx;
    rombp_patch_status local_status;

    FILE* input_file;
    FILE* output_file;
    FILE* patch_file;

    patch_status_init(&local_status);

    local_status.err = open_patch_files(&input_file, &output_file, &patch_file, command);
    if (local_status.err == PATCH_ERR_IO) {
        local_status.iter_status = HUNK_DONE;
        goto done;
    }
    patch_type = detect_patch_type(patch_file);
    if (patch_type == PATCH_TYPE_UNKNOWN) {
        local_status.iter_status = HUNK_DONE;
        local_status.err = PATCH_UNKNOWN_TYPE;
        goto done;
    }
    rc = start_patch(patch_type, &patch_ctx, input_file, patch_file, output_file);
    if (rc < 0) {
        local_status.iter_status = HUNK_DONE;
        local_status.err = PATCH_FAILED_TO_START;
        goto done;
    }
    local_status.iter_status = HUNK_NEXT;

    while (1) {
        switch (local_status.iter_status) {
            case HUNK_NEXT: {
                local_status.iter_status = next_hunk(patch_type, &patch_ctx, input_file, output_file, patch_file);
                if (local_status.iter_status == HUNK_NEXT) {
                    local_status.hunk_count++;
                    rombp_log_info("Got next hunk, hunk count: %d\n", local_status.hunk_count);
                }
                
                rombp_update_patch_status(status, &local_status);
                break;
            }
            case HUNK_DONE: {
                local_status.err = end_patch(patch_type, &patch_ctx, patch_file);
                goto done;
            }
            case HUNK_ERR_IO:
                local_status.err = PATCH_ERR_IO;
                rombp_log_err("I/O error during hunk iteration\n");
                goto done;
            case HUNK_NONE:
                break;
        }
    }

done:
    local_status.is_done = 1;
    close_files(input_file, output_file, patch_file);
    rombp_update_patch_status(status, &local_status);
    rombp_patch_err err = local_status.err;
    patch_status_destroy(&local_status);
    return err;
}

typedef struct rombp_patch_thread_args {
    rombp_patch_command* command;
    rombp_patch_status status;
    int rc;
} rombp_patch_thread_args;

static void* execute_patch_threaded(void* args) {
    rombp_patch_thread_args* patch_args = (rombp_patch_thread_args *)args;
    int rc = execute_patch(patch_args->command, &patch_args->status);
    if (rc != 0) {
        rombp_log_err("Threaded patch failed: %d\n", rc);
    }

    patch_args->rc = rc;
    return NULL;
}

// Start patching on a separate thread
static int rombp_start_patch_thread(pthread_t* patch_thread, rombp_patch_thread_args* thread_args) {
    int rc = pthread_create(patch_thread, NULL, &execute_patch_threaded, thread_args);
    if (rc != 0) {
        rombp_log_err("Failed to create patching thread: %d\n", rc);
        return rc;
    }
    return 0;
}

static int rombp_wait_patch_thread(pthread_t* patch_thread) {
    int rc = pthread_join(*patch_thread, NULL);
    if (rc != 0) {
        rombp_log_err("Failed to join patch thread: %d\n", rc);
        return rc;
    }
    return 0;
}

static int ui_loop(pthread_t* patch_thread, rombp_patch_command* command) {
    rombp_ui ui;
    int patching = 0;
    char tmp_buf[255];
    rombp_patch_status local_status;

    rombp_patch_thread_args thread_args;
    thread_args.command = command;
    thread_args.rc = 0;

    patch_status_init(&thread_args.status);
    patch_status_init(&local_status);

    int rc = ui_start(&ui);
    if (rc != 0) {
        rombp_log_err("Failed to start UI, error code: %d\n", rc);
        return 1;
    }

    while (1) {
        rombp_ui_event event = ui_handle_event(&ui, command);

        // First, handle user input
        switch (event) {
            case EV_QUIT:
                rc = 0;
                goto out;
            case EV_PATCH_COMMAND:
                patch_status_reset(&thread_args.status);
                rc = rombp_start_patch_thread(patch_thread, &thread_args);
                if (rc != 0) {
                    rombp_log_err("FATAL: Failed to start patch thread: %d\n", rc);
                    goto out;
                }
                patching = 1;
                break;
            default:
                break;
        }

        // Then, if we're patching, copy thread status to a local struct so we can check its current state
        if (patching) {
            rombp_read_patch_status(&thread_args.status, &local_status);
            switch (local_status.iter_status) {
                case HUNK_NEXT:
                    sprintf(tmp_buf, PATCH_NEXT_MESSAGE, local_status.hunk_count);
                    ui_status_bar_reset_text(&ui, &ui.bottom_bar, tmp_buf);
                    break;
                case HUNK_DONE:
                    if (local_status.is_done) {
                        switch (local_status.err) {
                            case PATCH_OK:
                                sprintf(tmp_buf, PATCH_SUCCESS_MESSAGE, local_status.hunk_count);
                                ui_status_bar_reset_text(&ui, &ui.bottom_bar, tmp_buf);
                                rombp_log_info("Done patching file, hunk count: %d\n", thread_args.status.hunk_count);
                                break;
                            case PATCH_INVALID_OUTPUT_SIZE:
                                ui_status_bar_reset_text(&ui, &ui.bottom_bar, PATCH_FAIL_INVALID_OUTPUT_SIZE_MESSAGE);
                                rombp_log_err("Invalid output size\n");
                                break;
                            case PATCH_INVALID_OUTPUT_CHECKSUM:
                                ui_status_bar_reset_text(&ui, &ui.bottom_bar, PATCH_FAIL_INVALID_OUTPUT_CHECKSUM_MESSAGE);
                                rombp_log_err("Invalid output checksum\n");
                                break;
                            case PATCH_ERR_IO:
                                ui_status_bar_reset_text(&ui, &ui.bottom_bar, PATCH_FAIL_ERR_IO);
                                rombp_log_err("Failed to open files for patching: %d\n", thread_args.status.err);
                                break;
                            case PATCH_UNKNOWN_TYPE:
                                ui_status_bar_reset_text(&ui, &ui.bottom_bar, PATCH_FAIL_UNKNOWN_TYPE);
                                rombp_log_err("Bad patch file type\n");
                                break;
                            case PATCH_FAILED_TO_START:
                                ui_status_bar_reset_text(&ui, &ui.bottom_bar, PATCH_FAIL_START);
                                rombp_log_err("Failed to start patching\n");
                                break;
                            default:
                                ui_status_bar_reset_text(&ui, &ui.bottom_bar, PATCH_UNKNOWN_ERROR_MESSAGE);
                                rombp_log_err("Unknown end error: %d\n", thread_args.status.err);
                                break;
                        }
                        rc = rombp_wait_patch_thread(patch_thread);
                        if (rc != 0) {
                            rombp_log_err("Could not wait for patch thread to stop: %d\n", rc);
                            return rc;
                        }
                        ui_free_command(command);
                        patching = 0;
                    }
                    break;
                case HUNK_ERR_IO:
                    ui_status_bar_reset_text(&ui, &ui.bottom_bar, "ERROR: IO error decoding next patch hunk");
                    rombp_log_err("I/O error during hunk iteration\n");
                    ui_free_command(command);
                    patching = 0;
                    break;
                case HUNK_NONE:
                default:
                    break;
            }
        }

        rc = ui_draw(&ui);
        if (rc != 0) {
            rombp_log_err("Failed to draw: %d\n", rc);
            goto out;
        }

        SDL_Delay(DEFAULT_SLEEP);
    }

out:
    ui_stop(&ui);
    patch_status_destroy(&thread_args.status);
    patch_status_destroy(&local_status);
    return rc;
}

static int execute_command_line(int argc, char** argv, pthread_t* patch_thread, rombp_patch_command* command) {
    int rc;

    rc = parse_command_line(argc, argv, command);
    if (rc != 0) {
        return rc;
    }

    rombp_patch_thread_args thread_args;
    thread_args.command = command;
    thread_args.rc = 0;
    patch_status_init(&thread_args.status);

    rc = rombp_start_patch_thread(patch_thread, &thread_args);
    if (rc != 0) {
        rombp_log_err("Could not start patch thread: %d\n", rc);
        return rc;
    }
    rc = rombp_wait_patch_thread(patch_thread);
    if (rc != 0) {
        rombp_log_err("Could not wait for patch thread to stop: %d\n", rc);
        return rc;
    }

    if (!thread_args.status.is_done) {
        rombp_log_err("Illegal state: The patching thread terminated, but did not register itself as done\n");
        return -1;
    }

    if (thread_args.rc != 0) {
        rombp_log_err("Patch thread returned non-zero error code: %d\n", thread_args.rc);
        return thread_args.rc;
    }

    switch (thread_args.status.err) {
        case PATCH_OK:
            rombp_log_info("Done patching file, hunk count: %d\n", thread_args.status.hunk_count);
            break;
        case PATCH_INVALID_OUTPUT_SIZE:
            rombp_log_err("Invalid output size\n");
            break;
        case PATCH_INVALID_OUTPUT_CHECKSUM:
            rombp_log_err("Invalid output checksum\n");
            break;
        case PATCH_ERR_IO:
            rombp_log_err("Failed to open files for patching: %d\n", thread_args.status.err);
            break;
        case PATCH_UNKNOWN_TYPE:
            rombp_log_err("Bad patch file type\n");
            break;
        case PATCH_FAILED_TO_START:
            rombp_log_err("Failed to start patching\n");
            break;
        default:
            rombp_log_err("Unknown end error: %d\n", thread_args.status.err);
            break;
    }

    patch_status_destroy(&thread_args.status);

    return 0;
}

int main(int argc, char** argv) {
    rombp_patch_command command;
    pthread_t patch_thread;

    command.input_file = NULL;
    command.ips_file = NULL;
    command.output_file = NULL;

    if (argc > 1) {
        // If the user passed command line arguments, assume they don't want to launch
        // the SDL UI.
        return execute_command_line(argc, argv, &patch_thread, &command);
    } else {
        int rc = ui_loop(&patch_thread, &command);
        if (rc != 0) {
            rombp_log_err("Failed to initiate UI loop: %d\n", rc);
            return -1;
        }
    }

    return 0;
}
