@ Copyright 2026 Samus Returns port
@ Licensed under GPLv2 or any later version
@ Refer to the license.txt file included.
@
@ Samus Returns (USA): swaps Samus between her suit models and the Fusion ones while the game
@ runs (msr/suit_swap.h). Placed in the unused end of the text segment; the frame limiter's
@ first instruction (0x156450, once per frame on the game thread) branches here while the
@ frontend has a command waiting in the mailbox. tools/msr/suit_swap/assemble.py turns this
@ into suit_swap_stub.h.
@
@ Mailbox: +0x00 armed, +0x04 command, +0x08 family (0 suits, 1 Fusion suits),
@          +0x0C component count, +0x10 components (up to 8), +0x30 commands run,
@          +0x40 call count, +0x44 calls (up to 4: function, r0, r1, s0).

.arm
.fpu vfpv2
.text
.global entry

.equ MAILBOX, 0x6C4F00
.equ RESUME, 0x156454           @ after the frame limiter's push, replayed below
.equ PACKAGE_MANAGER, 0x76F2E0  @ singleton pointer
.equ RESOURCE_MANAGER, 0x76F2D8 @ singleton pointer
.equ SUIT_PACKAGES, 0x70511C    @ common, power, varia, gravity (4-byte ids)
.equ FUSION_PACKAGES, 0x70512C  @ the same, _fusion
.equ PACKAGE_LISTED, 0x19024C   @ (manager, id): queued or loaded
.equ PACKAGE_QUEUE, 0x16CC84    @ (manager, id, 0, priority 9, [sp] 0.0f)
.equ PACKAGE_PROCESS, 0x151670  @ (manager): runs the queued requests
.equ PACKAGE_RELEASE, 0x16D2AC  @ (manager, id, 0)
.equ NAME_DIFFERS, 0x1D1534     @ (name, name)
.equ NAME_EQUALS, 0x1B3234      @ (name, name)
.equ RESOURCE_RELEASE, 0x18FA6C @ (resource manager, resource)
.equ NAME_DESTROY, 0x1B28E8
.equ ID_DESTROY, 0x1B3180
.equ APPLY_MODEL, 0x402E1C      @ (component, entry): replaces the model on screen

@ Model component (vtable 0x6DC880): +0x2E4 entries (32 bytes: name, +0x14 path, +0x18 extra,
@ +0x1C resource), +0x2E8 names (20 bytes), +0x2EC count, +0x30 model on screen, +0x300 its
@ name; vtable +0x18 registers the suit models (0x404068, by the Fusion flag).

entry:
    push {r0-r12, lr}
    ldr r4, =MAILBOX
    ldr r0, [r4]
    cmp r0, #0
    beq leave
    mov r0, #0
    str r0, [r4]
    ldr r5, [r4, #4]
    cmp r5, #1
    bleq load_family
    cmp r5, #2
    bleq swap_models
    cmp r5, #3
    bleq release_family
    cmp r5, #4
    bleq call_list
    ldr r0, [r4, #0x30]
    add r0, r0, #1
    str r0, [r4, #0x30]
leave:
    pop {r0-r12, lr}
    push {r4, r5, r6, lr}
    ldr pc, =RESUME

@ The family's base id in r8; r7 the package manager (0: none yet, Z set).
family:
    ldr r0, [r4, #8]
    cmp r0, #0
    ldreq r8, =SUIT_PACKAGES
    ldrne r8, =FUSION_PACKAGES
    ldr r7, =PACKAGE_MANAGER
    ldr r7, [r7]
    cmp r7, #0
    bx lr

@ Queues the family's four packages that are not there yet and loads them, as the game does
@ when a save starts (0x143EB8).
load_family:
    push {r4-r11, lr}
    sub sp, sp, #8
    bl family
    beq load_done
    mov r9, #0
load_next:
    mov r0, r7
    add r1, r8, r9, lsl #2
    ldr ip, =PACKAGE_LISTED
    blx ip
    cmp r0, #0
    bne load_skip
    str r0, [sp]
    mov r0, r7
    add r1, r8, r9, lsl #2
    mov r2, #0
    mov r3, #9
    ldr ip, =PACKAGE_QUEUE
    blx ip
load_skip:
    add r9, r9, #1
    cmp r9, #4
    blt load_next
    mov r0, r7
    ldr ip, =PACKAGE_PROCESS
    blx ip
load_done:
    add sp, sp, #8
    pop {r4-r11, pc}

@ Releases the family's four packages.
release_family:
    push {r4-r11, lr}
    bl family
    beq release_done
    mov r9, #0
release_next:
    mov r0, r7
    add r1, r8, r9, lsl #2
    mov r2, #0
    ldr ip, =PACKAGE_RELEASE
    blx ip
    add r9, r9, #1
    cmp r9, #4
    blt release_next
release_done:
    pop {r4-r11, pc}

@ Calls game functions with an object and a float, such as the setters that also tell the
@ HUD (the cheats' energy and Aeion).
call_list:
    push {r4-r11, lr}
    ldr r9, [r4, #0x40]
    add r10, r4, #0x44
call_next:
    cmp r9, #0
    beq call_done
    ldr r0, [r10, #4]
    ldr r1, [r10, #8]
    vldr s0, [r10, #12]
    ldr ip, [r10]
    blx ip
    add r10, r10, #16
    sub r9, r9, #1
    b call_next
call_done:
    pop {r4-r11, pc}

swap_models:
    push {r4-r11, lr}
    ldr r9, [r4, #0xC]
    add r10, r4, #0x10
swap_next:
    cmp r9, #0
    beq swap_done
    ldr r6, [r10], #4
    sub r9, r9, #1
    bl swap_one
    b swap_next
swap_done:
    pop {r4-r11, pc}

@ r6: a model component. Empties its model list as its destructor does (0x403260), registers
@ the models again, then puts the model on screen back by its name.
swap_one:
    push {r4-r11, lr}
    mov r7, #0
release_resources:
    ldr r0, [r6, #0x2EC]
    cmp r7, r0
    bhs destroy_names
    ldr r0, [r6, #0x2E4]
    add r8, r0, r7, lsl #5
    mov r0, r8
    add r1, r6, #0x300
    ldr ip, =NAME_DIFFERS
    blx ip
    cmp r0, #0
    beq release_skip
    ldr r0, =RESOURCE_MANAGER
    ldr r0, [r0]
    cmp r0, #0
    beq release_skip
    ldr r1, [r8, #0x1C]
    ldr ip, =RESOURCE_RELEASE
    blx ip
release_skip:
    add r7, r7, #1
    b release_resources
destroy_names:
    mov r7, #0
destroy_name:
    ldr r0, [r6, #0x2EC]
    cmp r7, r0
    bhs destroy_entries
    ldr r0, [r6, #0x2E8]
    add r1, r7, r7, lsl #2
    add r0, r0, r1, lsl #2
    ldr ip, =NAME_DESTROY
    blx ip
    add r7, r7, #1
    b destroy_name
destroy_entries:
    mov r7, #0
destroy_entry:
    ldr r0, [r6, #0x2EC]
    cmp r7, r0
    bhs register
    ldr r0, [r6, #0x2E4]
    add r8, r0, r7, lsl #5
    mov r0, #0
    str r0, [r8, #0x1C]
    add r0, r8, #0x18
    ldr ip, =ID_DESTROY
    blx ip
    add r0, r8, #0x14
    ldr ip, =ID_DESTROY
    blx ip
    mov r0, r8
    ldr ip, =NAME_DESTROY
    blx ip
    add r7, r7, #1
    b destroy_entry
register:
    mov r0, #0
    str r0, [r6, #0x2EC]
    ldr r0, [r6]
    ldr ip, [r0, #0x18]
    mov r0, r6
    blx ip
    ldr r0, [r6, #0x30]
    cmp r0, #0
    beq swap_one_done
    mov r7, #0
find_current:
    ldr r0, [r6, #0x2EC]
    cmp r7, r0
    bhs swap_one_done
    ldr r0, [r6, #0x2E8]
    add r1, r7, r7, lsl #2
    add r0, r0, r1, lsl #2
    add r1, r6, #0x300
    ldr ip, =NAME_EQUALS
    blx ip
    cmp r0, #0
    bne apply
    add r7, r7, #1
    b find_current
apply:
    ldr r0, [r6, #0x2E4]
    add r1, r0, r7, lsl #5
    mov r0, r6
    ldr ip, =APPLY_MODEL
    blx ip
swap_one_done:
    pop {r4-r11, pc}

.ltorg
