#include <cstdlib>
#define SLJIT_CONFIG_AUTO 1
#include "../src/sljitLir.h"
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <iostream>
#include <vector>
#include <unordered_map>
#include <fstream>
#include "../parser/JavaClass.h"
#include "../parser/JavaInstruction.h"
#include <chrono>
using namespace std;
using namespace parser;
#include "main.h"
#include <map>
#include <vector>
#include <iostream>
#include <iomanip>
#include <cstring>

struct JitContext {
    sljit_compiler* compiler;
    int stackBase;
    int stackPtr = 0;
    int maxLocals;
    int maxStack;
    map<int, sljit_label*> labels;
    vector<pair<sljit_jump*, int>> pendingJumps;

    JitContext(sljit_compiler* c, int ml, int ms) : compiler(c), maxLocals(ml), maxStack(ms) {
        stackBase = maxLocals * sizeof (sljit_sw);
    }

    /* ==================== INT операции (32-бит) ====================*/
    void pushInt(sljit_s32 src, sljit_sw srcw) {
        /* ВАЖНО: Используем SLJIT_MOV32 для чистой 32-битной операции*/ /* без sign-extension*/
        sljit_emit_op1(compiler, SLJIT_MOV32, SLJIT_MEM1(SLJIT_SP), stackBase + stackPtr * sizeof (sljit_sw), src, srcw);
        stackPtr++;
    }

    void popInt(sljit_s32 dst, sljit_sw dstw) {
        stackPtr--;
        /* SLJIT_MOV32 берет 32 бита без sign-extension*/
        sljit_emit_op1(compiler, SLJIT_MOV32, dst, dstw, SLJIT_MEM1(SLJIT_SP), stackBase + stackPtr * sizeof (sljit_sw));
    }

    void loadLocalInt(int idx, sljit_s32 dst, sljit_sw dstw) {
        sljit_emit_op1(compiler, SLJIT_MOV32, dst, dstw, SLJIT_MEM1(SLJIT_SP), idx * sizeof (sljit_sw));
    }

    void storeLocalInt(int idx, sljit_s32 src, sljit_sw srcw) {
        sljit_emit_op1(compiler, SLJIT_MOV32, SLJIT_MEM1(SLJIT_SP), idx * sizeof (sljit_sw), src, srcw);
    }

    /* ==================== LONG операции (64-бит) ====================*/
    void pushLong(sljit_s32 src, sljit_sw srcw) {
        /* ВАЖНО: SLJIT_MOV для 64-бит*/
        sljit_emit_op1(compiler, SLJIT_MOV, SLJIT_MEM1(SLJIT_SP), stackBase + stackPtr * sizeof (sljit_sw), src, srcw);
        stackPtr += 2;
        /* long занимает 2 слота*/
    }

    void popLong(sljit_s32 dst, sljit_sw dstw) {
        stackPtr -= 2;
        sljit_emit_op1(compiler, SLJIT_MOV, dst, dstw, SLJIT_MEM1(SLJIT_SP), stackBase + stackPtr * sizeof (sljit_sw));
    }

    void loadLocalLong(int idx, sljit_s32 dst, sljit_sw dstw) {
        /* ВАЖНО: SLJIT_MOV для 64-бит!*/
        sljit_emit_op1(compiler, SLJIT_MOV, dst, dstw, SLJIT_MEM1(SLJIT_SP), idx * sizeof (sljit_sw));
    }

    void storeLocalLong(int idx, sljit_s32 src, sljit_sw srcw) {
        /* ВАЖНО: SLJIT_MOV для 64-бит!*/
        sljit_emit_op1(compiler, SLJIT_MOV, SLJIT_MEM1(SLJIT_SP), idx * sizeof (sljit_sw), src, srcw);
    }

    /* ==================== DOUBLE операции ====================*/
    void pushDouble(sljit_s32 src, sljit_sw srcw) {
        sljit_emit_fop1(compiler, SLJIT_MOV_F64, SLJIT_MEM1(SLJIT_SP), stackBase + stackPtr * sizeof (sljit_sw), src, srcw);
        stackPtr += 2;
    }

    void popDouble(sljit_s32 dst, sljit_sw dstw) {
        stackPtr -= 2;
        sljit_emit_fop1(compiler, SLJIT_MOV_F64, dst, dstw, SLJIT_MEM1(SLJIT_SP), stackBase + stackPtr * sizeof (sljit_sw));
    }

    void pushDoubleConst(double value) {
        sljit_emit_fset64(compiler, SLJIT_FR0, value);
        sljit_emit_fop1(compiler, SLJIT_MOV_F64, SLJIT_MEM1(SLJIT_SP), stackBase + stackPtr * sizeof (sljit_sw), SLJIT_FR0, 0);
        stackPtr += 2;
    }

    void loadLocalDouble(int idx, sljit_s32 dst, sljit_sw dstw) {
        sljit_emit_fop1(compiler, SLJIT_MOV_F64, dst, dstw, SLJIT_MEM1(SLJIT_SP), idx * sizeof (sljit_sw));
    }

    void storeLocalDouble(int idx, sljit_s32 src, sljit_sw srcw) {
        sljit_emit_fop1(compiler, SLJIT_MOV_F64, SLJIT_MEM1(SLJIT_SP), idx * sizeof (sljit_sw), src, srcw);
    }

    /* ==================== Вспомогательные ====================*/
    void adjustStack(int slots) {
        stackPtr += slots;
    }

    void bindLabel(int pc) {
        auto it = labels.find(pc);
        if (it != labels.end() && it->second == nullptr) {
            labels[pc] = sljit_emit_label(compiler);
        } else if (it == labels.end()) {
            labels[pc] = sljit_emit_label(compiler);
        }
    }

    void addPendingJump(sljit_jump* jump, int targetPc) {
        pendingJumps.push_back({jump, targetPc});
    }

    void resolvePendingJumps() {
        cout << "=== Resolving " << pendingJumps.size() << " pending jumps ===\n";
        for (auto& p : pendingJumps) {
            sljit_jump* jump = p.first;
            int targetPc = p.second;
            auto it = labels.find(targetPc);
            if (it != labels.end() && it->second != nullptr) {
                sljit_set_label(jump, it->second);
            } else {
                std::cerr << "ERROR: Unresolved jump -> pc " << targetPc << "\n";
            }
        }
    }
};

// Один элемент таблицы переходов

struct V2PTable {
    int32_t value; // Значение из case (например, 10, 100, 500)
    int32_t bc_pos; // Абсолютный индекс в байт-коде (pc + offset)
    void* jump_ptr; // Сюда JIT подставит адрес sljit_label
};

// Контейнер для таблицы

struct SwitchTable {
    int32_t count; // Количество элементов (npairs или high-low+1)
    V2PTable table[]; // Flexible array member (C99)
};

SwitchTable* switchtable_create(int n) {
    // Выделяем память под заголовок + массив из N элементов
    size_t size = sizeof (SwitchTable) + (sizeof (V2PTable) * n);

    SwitchTable* st = (SwitchTable*) malloc(size);
    if (!st) return nullptr;

    st->count = n;

    // Обнуляем память для безопасности
    memset(st->table, 0, sizeof (V2PTable) * n);

    // В реальной JVM мы бы добавили st в список для последующего освобождения 
    // вместе с методом (gc_register_jit_table(jit_context, st))

    return st;
}

void genCode(sljit_compiler* compiler) {
    JavaClass clazz("D:\\BackupWinda\\Documents\\NetBeansProjects\\ButkaJitTest\\build\\classes\\Budka.class");
    JavaMethod method = findTestMethod(clazz);

    auto code_attr = method->getCode();
    if (!code_attr) {
        cerr << "No Code attribute\n";
        return;
    }

    auto& bc = code_attr->code;

    int max_locals = code_attr->maxLocals;
    int max_stack = code_attr->maxStack;

    // Для long значений нужно учитывать, что они занимают 2 слота
    // Стек должен поддерживать как int (4 байта), так и long (8 байт)
    int frame = (max_locals * 2 + max_stack * 2) * sizeof (sljit_sw);

    // Возвращаем WORD (long), принимаем два 32-битных int
    // SLJIT_ARGS2(W, 32, 32) означает: возврат Word, два аргумента по 32 бита
    sljit_emit_enter(compiler, 0, SLJIT_ARGS2(W, 32, 32),
            5 | SLJIT_ENTER_FLOAT(4), 2, frame);

    // Сохраняем аргументы int n (S0) и int m (S1) в локальные переменные
    // int занимает 4 байта, но для совместимости с long лучше использовать 8-байтовые слоты
    sljit_emit_op1(compiler, SLJIT_MOV_S32,
            SLJIT_MEM1(SLJIT_SP), 0 * sizeof (sljit_sw), SLJIT_S0, 0);
    sljit_emit_op1(compiler, SLJIT_MOV_S32,
            SLJIT_MEM1(SLJIT_SP), 1 * sizeof (sljit_sw), SLJIT_S1, 0);

    //    sljit_emit_enter(compiler, 0, SLJIT_ARGS2(W, 32, 32), 5 | SLJIT_ENTER_FLOAT(4), 2, frame);
    //    sljit_emit_enter(compiler, 0, SLJIT_ARGS2(F64, W, W), 5 | SLJIT_ENTER_FLOAT(4), 2, frame);

    //    sljit_emit_op1(compiler, SLJIT_MOV_S32, SLJIT_MEM1(SLJIT_SP), 0 * sizeof (sljit_s32), SLJIT_S0, 0);
    //    sljit_emit_op1(compiler, SLJIT_MOV_S32, SLJIT_MEM1(SLJIT_SP), 1 * sizeof (sljit_s32), SLJIT_S1, 0);

    JitContext ctx(compiler, max_locals, max_stack);

    cout << "=== Scanning for jump targets ===\n";

    for (size_t pc = 0; pc < bc.size();) {
        uint8_t op = bc[pc];
        auto inst = static_cast<JavaInstruction> (op);
        auto& info = lookupInstruction(inst);

        switch (inst) {
            case JavaInstruction::Goto:
            case JavaInstruction::Ifne:
            case JavaInstruction::If_icmpge:
            case JavaInstruction::If_icmple:
            case JavaInstruction::If_icmplt:
            case JavaInstruction::If_icmpgt:
            case JavaInstruction::If_icmpne:
            case JavaInstruction::If_icmpeq:
            case JavaInstruction::Ifeq:
            case JavaInstruction::Iflt:
            case JavaInstruction::Ifle:
            case JavaInstruction::Ifgt:
            case JavaInstruction::Ifge:
            {
                if (pc + 2 < bc.size()) {
                    int16_t off = (int16_t) ((bc[pc + 1] << 8) | bc[pc + 2]);
                    int target = pc + off;
                    if (ctx.labels.find(target) == ctx.labels.end()) {
                        ctx.labels[target] = nullptr;
                    }
                    cout << " Jump at " << pc << " -> target " << target << "\n";
                }
                pc += 3;
                break;
            }

            case JavaInstruction::IStore:
            case JavaInstruction::ILoad:
            case JavaInstruction::LStore: // Добавьте это
            case JavaInstruction::LLoad: // И это
            case JavaInstruction::DStore:
            case JavaInstruction::DLoad:
            case JavaInstruction::Ldc:
            {
                pc += 2;
                break;
            }

            case JavaInstruction::Ldc_w:
            case JavaInstruction::Ldc2_w:
            case JavaInstruction::Iinc:
            {
                pc += 3;
                break;
            }
            case JavaInstruction::TableSwitch:
            {
                // 1. Выравнивание (padding)
                // Данные начинаются с адреса, кратного 4 относительно начала метода
                int padding = (4 - (pc + 1) % 4) % 4;
                int current_pos = pc + 1 + padding;

                // Вспомогательная лямбда для чтения s32 из вектора байт-кода
                auto get_s32_scan = [&](int offset) {
                    return (int32_t) ((bc[offset] << 24) | (bc[offset + 1] << 16) |
                            (bc[offset + 2] << 8) | bc[offset + 3]);
                };

                // 2. Читаем заголовок таблицы
                int32_t default_offset = get_s32_scan(current_pos);
                int32_t low = get_s32_scan(current_pos + 4);
                int32_t high = get_s32_scan(current_pos + 8);
                int32_t num_targets = high - low + 1;

                // 3. Регистрируем метку для Default case
                int default_target = pc + default_offset;
                ctx.labels[default_target] = nullptr;
                cout << "  TableSwitch at " << pc << " -> default target " << default_target << "\n";

                // 4. Регистрируем метки для всех веток таблицы
                int jump_table_base = current_pos + 12;
                for (int i = 0; i < num_targets; i++) {
                    int32_t offset = get_s32_scan(jump_table_base + i * 4);
                    int target = pc + offset;
                    ctx.labels[target] = nullptr;
                    cout << "  TableSwitch at " << pc << " -> case target " << target << "\n";
                }

                // 5. КРИТИЧНО: Продвигаем PC на правильную длину всей инструкции
                // 1 (opcode) + padding + 12 (def, low, high) + 4 * количество веток
                pc = jump_table_base + (num_targets * 4);
                break;
            }
            case JavaInstruction::LookupSwitch:
            {
                // 1. Выравнивание (padding) относительно начала метода
                int padding = (4 - (pc + 1) % 4) % 4;
                int current_pos = pc + 1 + padding;

                // Лямбда для чтения s32 (Big Endian)
                auto get_s32_scan = [&](int offset) {
                    return (int32_t) ((bc[offset] << 24) | (bc[offset + 1] << 16) | (bc[offset + 2] << 8) | bc[offset + 3]);
                };

                // 2. Читаем default offset и количество пар npairs
                int32_t default_offset = get_s32_scan(current_pos);
                int32_t npairs = get_s32_scan(current_pos + 4);

                // 3. Регистрируем метку для Default
                int default_target = pc + default_offset;
                if (ctx.labels.find(default_target) == ctx.labels.end()) {
                    ctx.labels[default_target] = nullptr;
                }

                // 4. Проходим по всем парам и регистрируем их метки
                // Каждая пара занимает 8 байт (4 байта ключ, 4 байта смещение)
                int pairs_base = current_pos + 8;
                for (int i = 0; i < npairs; i++) {
                    // Смещение цели перехода находится во втором s32 каждой пары
                    int32_t offset = get_s32_scan(pairs_base + i * 8 + 4);
                    int target = pc + offset;
                    if (ctx.labels.find(target) == ctx.labels.end()) {
                        ctx.labels[target] = nullptr;
                    }
                }

                // 5. Перепрыгиваем всю инструкцию
                // 1 (opcode) + padding + 4 (default) + 4 (npairs) + 8 * npairs
                pc = pairs_base + (npairs * 8);
                break;
            }


            default:
            {
                pc += info.length ? info.length : 1;
                break;
            }
        }
    }

    ctx.stackPtr = 0;

    cout << "=== JIT generation ===\n";

    for (size_t pc = 0; pc < bc.size();) {
        uint8_t op = bc[pc];
        auto inst = static_cast<JavaInstruction> (op);
        auto& info = lookupInstruction(inst);

        if (ctx.labels.find(pc) != ctx.labels.end()) {
            ctx.bindLabel(pc);
        }

        sljit_emit_op0(compiler, SLJIT_NOP);
        cout << pc << "\t" << info.name << "\n";

        switch (inst) {
            case JavaInstruction::Nop:
            {
                pc++;
                break;
            }

            case JavaInstruction::TableSwitch:
            {
                int padding = (4 - (pc + 1) % 4) % 4;
                int current_pos = pc + 1 + padding;

                auto get_s32 = [&](int offset) {
                    return (int32_t) ((bc[offset] << 24) | (bc[offset + 1] << 16) |
                            (bc[offset + 2] << 8) | bc[offset + 3]);
                };

                int32_t default_offset = get_s32(current_pos);
                int32_t low = get_s32(current_pos + 4);
                int32_t high = get_s32(current_pos + 8);
                int jump_table_base = current_pos + 12;
                int32_t num_targets = high - low + 1;

                // 1. Получаем значение из стека
                ctx.popInt(SLJIT_R0, 0);

                // 2. Проверка границ: if (val < low || val > high) goto default
                auto jump_less = sljit_emit_cmp(compiler, SLJIT_SIG_LESS,
                        SLJIT_R0, 0, SLJIT_IMM, low);
                auto jump_greater = sljit_emit_cmp(compiler, SLJIT_SIG_GREATER,
                        SLJIT_R0, 0, SLJIT_IMM, high);

                // 3. Нормализуем индекс: R0 = R0 - low
                sljit_emit_op2(compiler, SLJIT_SUB, SLJIT_R0, 0,
                        SLJIT_R0, 0, SLJIT_IMM, low);

                // 4. Создаем таблицу адресов в runtime
                // Выделяем память для таблицы меток (это нужно сделать ДО генерации кода)
                std::vector<sljit_label*> case_labels(num_targets);

                // 5. Генерируем код для каждого case и сохраняем метки
                std::vector<sljit_jump*> case_jumps;
                for (int i = 0; i < num_targets; i++) {
                    // Проверяем: if (R0 == i) goto case_i
                    auto j = sljit_emit_cmp(compiler, SLJIT_EQUAL,
                            SLJIT_R0, 0, SLJIT_IMM, i);

                    int32_t offset = get_s32(jump_table_base + i * 4);
                    int target_pc = pc + offset;
                    ctx.addPendingJump(j, target_pc);
                }

                // 6. Если ни один case не сработал, переходим к default
                // (это дублирование проверки границ для безопасности)
                auto jump_to_default = sljit_emit_jump(compiler, SLJIT_JUMP);
                ctx.addPendingJump(jump_to_default, pc + default_offset);

                // 7. Привязываем метки для out-of-bounds случаев
                sljit_set_label(jump_less, sljit_emit_label(compiler));
                sljit_set_label(jump_greater, sljit_emit_label(compiler));

                // Оба прыгают на default
                auto final_default_jump = sljit_emit_jump(compiler, SLJIT_JUMP);
                ctx.addPendingJump(final_default_jump, pc + default_offset);

                pc = jump_table_base + num_targets * 4;
                break;
            }
            case JavaInstruction::LookupSwitch:
            {
                // 1. Вычисление выравнивания (padding)
                int padding = (4 - (pc + 1) % 4) % 4;
                int current_pos = pc + 1 + padding;

                auto get_s32 = [&](int offset) {
                    return (int32_t) ((bc[offset] << 24) | (bc[offset + 1] << 16) |
                            (bc[offset + 2] << 8) | bc[offset + 3]);
                };

                // 2. Чтение параметров
                int32_t default_offset = get_s32(current_pos);
                int32_t npairs = get_s32(current_pos + 4);
                int pairs_base = current_pos + 8;

                // 3. Получаем значение для сравнения из стека Java
                ctx.popInt(SLJIT_R0, 0); // Искомое значение теперь в R0

                // 4. Генерируем сравнение для каждой пары (key, offset)
                for (int i = 0; i < npairs; i++) {
                    int32_t key = get_s32(pairs_base + i * 8);
                    int32_t offset = get_s32(pairs_base + i * 8 + 4);
                    int target_pc = pc + offset;

                    // Если R0 == key, прыгаем на target_pc
                    auto jump_match = sljit_emit_cmp(compiler, SLJIT_EQUAL, SLJIT_R0, 0, SLJIT_IMM, key);
                    ctx.addPendingJump(jump_match, target_pc);
                }

                // 5. Если ни один ключ не подошел, выполняем прыжок по умолчанию (Default)
                auto jump_default = sljit_emit_jump(compiler, SLJIT_JUMP);
                ctx.addPendingJump(jump_default, pc + default_offset);

                // 6. Обновление PC
                // Размер: 1 (опкод) + padding + 4 (default) + 4 (npairs) + 8 * npairs
                pc = pairs_base + (npairs * 8);
                break;
            }

            case JavaInstruction::Dcmpl:
            case JavaInstruction::Dcmpg:
            {
                bool is_dcmpg = (inst == JavaInstruction::Dcmpg);

                ctx.popDouble(SLJIT_FR1, 0); // value2
                ctx.popDouble(SLJIT_FR0, 0); // value1

                // Если хотя бы одно число NaN, прыгаем на метку nan
                auto jump_nan = sljit_emit_fcmp(compiler, SLJIT_UNORDERED, SLJIT_FR0, 0, SLJIT_FR1, 0);

                // 2. Проверка на равенство (v1 == v2)
                auto jump_equal = sljit_emit_fcmp(compiler, SLJIT_F_EQUAL, SLJIT_FR0, 0, SLJIT_FR1, 0);

                // 3. Проверка v1 < v2
                auto jump_less = sljit_emit_fcmp(compiler, SLJIT_F_LESS, SLJIT_FR0, 0, SLJIT_FR1, 0);

                // --- Результат: Больше (v1 > v2) ---
                sljit_emit_op1(compiler, SLJIT_MOV32, SLJIT_R0, 0, SLJIT_IMM, 1);
                auto jump_end = sljit_emit_jump(compiler, SLJIT_JUMP);

                // --- Результат: Меньше (v1 < v2) ---
                sljit_set_label(jump_less, sljit_emit_label(compiler));
                sljit_emit_op1(compiler, SLJIT_MOV32, SLJIT_R0, 0, SLJIT_IMM, -1);
                auto jump_end2 = sljit_emit_jump(compiler, SLJIT_JUMP);

                // --- Результат: Равно (v1 == v2) ---
                sljit_set_label(jump_equal, sljit_emit_label(compiler));
                sljit_emit_op1(compiler, SLJIT_MOV32, SLJIT_R0, 0, SLJIT_IMM, 0);
                auto jump_end3 = sljit_emit_jump(compiler, SLJIT_JUMP);

                // --- Результат: NaN ---
                sljit_set_label(jump_nan, sljit_emit_label(compiler));
                // dcmpl возвращает -1 при NaN, dcmpg возвращает 1
                sljit_emit_op1(compiler, SLJIT_MOV32, SLJIT_R0, 0, SLJIT_IMM, is_dcmpg ? 1 : -1);

                // Точка выхода для всех случаев
                auto label_done = sljit_emit_label(compiler);
                sljit_set_label(jump_end, label_done);
                sljit_set_label(jump_end2, label_done);
                sljit_set_label(jump_end3, label_done);

                ctx.pushInt(SLJIT_R0, 0);
                pc++;
                break;
            }

            case JavaInstruction::Lcmp:
            {
                ctx.popLong(SLJIT_R1, 0); // value2
                ctx.popLong(SLJIT_R0, 0); // value1

                // Lcmp сравнивает два long ЗНАКОВО и возвращает:
                // -1 если value1 < value2
                //  0 если value1 == value2
                //  1 если value1 > value2

                // Сначала проверяем равенство
                auto jump_equal = sljit_emit_cmp(compiler, SLJIT_EQUAL, SLJIT_R0, 0, SLJIT_R1, 0);

                // Затем проверяем меньше (ЗНАКОВОЕ!)
                auto jump_less = sljit_emit_cmp(compiler, SLJIT_SIG_LESS, SLJIT_R0, 0, SLJIT_R1, 0);

                // Если не равны и не меньше, значит больше
                // Больше: возвращаем 1
                sljit_emit_op1(compiler, SLJIT_MOV32, SLJIT_R0, 0, SLJIT_IMM, 1);
                auto jump_end_greater = sljit_emit_jump(compiler, SLJIT_JUMP);

                // Меньше: возвращаем -1
                sljit_set_label(jump_less, sljit_emit_label(compiler));
                sljit_emit_op1(compiler, SLJIT_MOV32, SLJIT_R0, 0, SLJIT_IMM, -1);
                auto jump_end_less = sljit_emit_jump(compiler, SLJIT_JUMP);

                // Равны: возвращаем 0
                sljit_set_label(jump_equal, sljit_emit_label(compiler));
                sljit_emit_op1(compiler, SLJIT_MOV32, SLJIT_R0, 0, SLJIT_IMM, 0);

                // Конец всех путей
                auto label_end = sljit_emit_label(compiler);
                sljit_set_label(jump_end_greater, label_end);
                sljit_set_label(jump_end_less, label_end);

                ctx.pushInt(SLJIT_R0, 0);
                pc++;
                break;
            }

            case JavaInstruction::Ifeq:
            {
                if (pc + 2 >= bc.size()) {
                    break;
                }
                int16_t off = (int16_t) ((bc[pc + 1] << 8) | bc[pc + 2]);
                int target = pc + off;
                ctx.popInt(SLJIT_R0, 0);
                auto jump = sljit_emit_cmp(compiler, SLJIT_EQUAL | SLJIT_32, SLJIT_R0, 0, SLJIT_IMM, 0);
                if (jump) {
                    ctx.addPendingJump(jump, target);
                }
                pc += 3;
                break;
            }

            case JavaInstruction::Ifne:
            {
                if (pc + 2 >= bc.size()) {
                    break;
                }
                int16_t off = (int16_t) ((bc[pc + 1] << 8) | bc[pc + 2]);
                int target = pc + off;
                ctx.popInt(SLJIT_R0, 0);
                auto jump = sljit_emit_cmp(compiler, SLJIT_NOT_EQUAL | SLJIT_32, SLJIT_R0, 0, SLJIT_IMM, 0);
                if (jump) {
                    ctx.addPendingJump(jump, target);
                }
                pc += 3;
                break;
            }

            case JavaInstruction::Iflt:
            {
                if (pc + 2 >= bc.size()) {
                    break;
                }
                int16_t off = (int16_t) ((bc[pc + 1] << 8) | bc[pc + 2]);
                int target = pc + off;
                ctx.popInt(SLJIT_R0, 0);
                auto jump = sljit_emit_cmp(compiler, SLJIT_SIG_LESS | SLJIT_32, SLJIT_R0, 0, SLJIT_IMM, 0);
                if (jump) {
                    ctx.addPendingJump(jump, target);
                }
                pc += 3;
                break;
            }

            case JavaInstruction::Ifle:
            {
                if (pc + 2 >= bc.size()) {
                    break;
                }
                int16_t off = (int16_t) ((bc[pc + 1] << 8) | bc[pc + 2]);
                int target = pc + off;
                ctx.popInt(SLJIT_R0, 0);
                auto jump = sljit_emit_cmp(compiler, SLJIT_SIG_LESS_EQUAL | SLJIT_32, SLJIT_R0, 0, SLJIT_IMM, 0);
                if (jump) {
                    ctx.addPendingJump(jump, target);
                }
                pc += 3;
                break;
            }

            case JavaInstruction::Ifgt:
            {
                if (pc + 2 >= bc.size()) {
                    break;
                }
                int16_t off = (int16_t) ((bc[pc + 1] << 8) | bc[pc + 2]);
                int target = pc + off;
                ctx.popInt(SLJIT_R0, 0);
                auto jump = sljit_emit_cmp(compiler, SLJIT_SIG_GREATER | SLJIT_32, SLJIT_R0, 0, SLJIT_IMM, 0);
                if (jump) {
                    ctx.addPendingJump(jump, target);
                }
                pc += 3;
                break;
            }

            case JavaInstruction::Ifge:
            {
                if (pc + 2 >= bc.size()) {
                    break;
                }
                int16_t off = (int16_t) ((bc[pc + 1] << 8) | bc[pc + 2]);
                int target = pc + off;
                ctx.popInt(SLJIT_R0, 0);
                auto jump = sljit_emit_cmp(compiler, SLJIT_SIG_GREATER_EQUAL | SLJIT_32, SLJIT_R0, 0, SLJIT_IMM, 0);
                if (jump) {
                    ctx.addPendingJump(jump, target);
                }
                pc += 3;
                break;
            }

            case JavaInstruction::If_icmpeq:
            {
                if (pc + 2 >= bc.size()) {
                    break;
                }
                int16_t off = (int16_t) ((bc[pc + 1] << 8) | bc[pc + 2]);
                int target = pc + off;
                ctx.popInt(SLJIT_R1, 0);
                ctx.popInt(SLJIT_R0, 0);
                auto jump = sljit_emit_cmp(compiler, SLJIT_EQUAL | SLJIT_32, SLJIT_R0, 0, SLJIT_R1, 0);
                if (jump) {
                    ctx.addPendingJump(jump, target);
                }
                pc += 3;
                break;
            }

            case JavaInstruction::If_icmpne:
            {
                if (pc + 2 >= bc.size()) {
                    break;
                }
                int16_t off = (int16_t) ((bc[pc + 1] << 8) | bc[pc + 2]);
                int target = pc + off;
                ctx.popInt(SLJIT_R1, 0);
                ctx.popInt(SLJIT_R0, 0);
                auto jump = sljit_emit_cmp(compiler, SLJIT_NOT_EQUAL | SLJIT_32, SLJIT_R0, 0, SLJIT_R1, 0);
                if (jump) {
                    ctx.addPendingJump(jump, target);
                }
                pc += 3;
                break;
            }

            case JavaInstruction::If_icmplt:
            {
                if (pc + 2 >= bc.size()) {
                    break;
                }
                int16_t off = (int16_t) ((bc[pc + 1] << 8) | bc[pc + 2]);
                int target = pc + off;
                ctx.popInt(SLJIT_R1, 0);
                ctx.popInt(SLJIT_R0, 0);
                auto jump = sljit_emit_cmp(compiler, SLJIT_SIG_LESS | SLJIT_32, SLJIT_R0, 0, SLJIT_R1, 0);
                if (jump) {
                    ctx.addPendingJump(jump, target);
                }
                pc += 3;
                break;
            }

            case JavaInstruction::If_icmpgt:
            {
                if (pc + 2 >= bc.size()) {
                    break;
                }
                int16_t off = (int16_t) ((bc[pc + 1] << 8) | bc[pc + 2]);
                int target = pc + off;
                ctx.popInt(SLJIT_R1, 0);
                ctx.popInt(SLJIT_R0, 0);
                auto jump = sljit_emit_cmp(compiler, SLJIT_SIG_GREATER | SLJIT_32, SLJIT_R0, 0, SLJIT_R1, 0);
                if (jump) {
                    ctx.addPendingJump(jump, target);
                }
                pc += 3;
                break;
            }

            case JavaInstruction::If_icmple:
            {
                if (pc + 2 >= bc.size()) {
                    break;
                }
                int16_t off = (int16_t) ((bc[pc + 1] << 8) | bc[pc + 2]);
                int target = pc + off;
                ctx.popInt(SLJIT_R1, 0);
                ctx.popInt(SLJIT_R0, 0);
                auto jump = sljit_emit_cmp(compiler, SLJIT_SIG_LESS_EQUAL | SLJIT_32, SLJIT_R0, 0, SLJIT_R1, 0);
                if (jump) {
                    ctx.addPendingJump(jump, target);
                }
                pc += 3;
                break;
            }

            case JavaInstruction::If_icmpge:
            {
                if (pc + 2 >= bc.size()) {
                    break;
                }
                int16_t off = (int16_t) ((bc[pc + 1] << 8) | bc[pc + 2]);
                int target = pc + off;
                ctx.popInt(SLJIT_R1, 0);
                ctx.popInt(SLJIT_R0, 0);
                auto jump = sljit_emit_cmp(compiler, SLJIT_SIG_GREATER_EQUAL | SLJIT_32, SLJIT_R0, 0, SLJIT_R1, 0);
                if (jump) {
                    ctx.addPendingJump(jump, target);
                }
                pc += 3;
                break;
            }



            case JavaInstruction::LStore:
            {
                if (pc + 1 >= bc.size()) break;

                int idx = bc[pc + 1]; // ВАЖНО: Это индекс СЛОТА, не байта!

                ctx.popLong(SLJIT_R0, 0);
                ctx.storeLocalLong(idx, SLJIT_R0, 0);

                std::cout << "\t\tlstore #" << idx << "\n";

                pc += 2;
                break;
            }

            case JavaInstruction::LLoad:
            {
                if (pc + 1 >= bc.size()) break;

                int idx = bc[pc + 1];

                ctx.loadLocalLong(idx, SLJIT_R0, 0);
                ctx.pushLong(SLJIT_R0, 0);

                std::cout << "\t\tlload #" << idx << "\n";

                pc += 2;
                break;
            }





            case JavaInstruction::ILoad:
            {
                if (pc + 1 >= bc.size()) {
                    break;
                }
                int idx = bc[pc + 1];
                ctx.loadLocalInt(idx, SLJIT_R0, 0);
                ctx.pushInt(SLJIT_R0, 0);
                pc += 2;
                break;
            }

            case JavaInstruction::IStore:
            {
                if (pc + 1 >= bc.size()) {
                    break;
                }
                int idx = bc[pc + 1];
                ctx.popInt(SLJIT_R0, 0);
                ctx.storeLocalInt(idx, SLJIT_R0, 0);
                pc += 2;
                break;
            }

            case JavaInstruction::Ishl:
            {
                ctx.popInt(SLJIT_R1, 0);
                ctx.popInt(SLJIT_R0, 0);
                sljit_emit_op2(compiler, SLJIT_AND | SLJIT_32, SLJIT_R1, 0, SLJIT_R1, 0, SLJIT_IMM, 0x1F);
                sljit_emit_op2(compiler, SLJIT_SHL | SLJIT_32, SLJIT_R0, 0, SLJIT_R0, 0, SLJIT_R1, 0);
                ctx.pushInt(SLJIT_R0, 0);
                pc++;
                break;
            }

            case JavaInstruction::Ishr:
            {
                ctx.popInt(SLJIT_R1, 0);
                ctx.popInt(SLJIT_R0, 0);
                sljit_emit_op2(compiler, SLJIT_AND | SLJIT_32, SLJIT_R1, 0, SLJIT_R1, 0, SLJIT_IMM, 0x1F);
                sljit_emit_op2(compiler, SLJIT_ASHR | SLJIT_32, SLJIT_R0, 0, SLJIT_R0, 0, SLJIT_R1, 0);
                ctx.pushInt(SLJIT_R0, 0);
                pc++;
                break;
            }

            case JavaInstruction::Iushr:
            {
                ctx.popInt(SLJIT_R1, 0);
                ctx.popInt(SLJIT_R0, 0);
                sljit_emit_op2(compiler, SLJIT_AND | SLJIT_32, SLJIT_R1, 0, SLJIT_R1, 0, SLJIT_IMM, 0x1F);
                sljit_emit_op2(compiler, SLJIT_LSHR | SLJIT_32, SLJIT_R0, 0, SLJIT_R0, 0, SLJIT_R1, 0);
                ctx.pushInt(SLJIT_R0, 0);
                pc++;
                break;
            }

            case JavaInstruction::Iand:
            {
                ctx.popInt(SLJIT_R1, 0);
                ctx.popInt(SLJIT_R0, 0);
                sljit_emit_op2(compiler, SLJIT_AND | SLJIT_32, SLJIT_R0, 0, SLJIT_R0, 0, SLJIT_R1, 0);
                ctx.pushInt(SLJIT_R0, 0);
                pc++;
                break;
            }

            case JavaInstruction::Ior:
            {
                ctx.popInt(SLJIT_R1, 0);
                ctx.popInt(SLJIT_R0, 0);
                sljit_emit_op2(compiler, SLJIT_OR | SLJIT_32, SLJIT_R0, 0, SLJIT_R0, 0, SLJIT_R1, 0);
                ctx.pushInt(SLJIT_R0, 0);
                pc++;
                break;
            }

            case JavaInstruction::Ixor:
            {
                ctx.popInt(SLJIT_R1, 0);
                ctx.popInt(SLJIT_R0, 0);
                sljit_emit_op2(compiler, SLJIT_XOR | SLJIT_32, SLJIT_R0, 0, SLJIT_R0, 0, SLJIT_R1, 0);
                ctx.pushInt(SLJIT_R0, 0);
                pc++;
                break;
            }

            case JavaInstruction::Lshl:
            {
                ctx.popInt(SLJIT_R1, 0);
                ctx.popLong(SLJIT_R0, 0);
                sljit_emit_op2(compiler, SLJIT_AND | SLJIT_32, SLJIT_R1, 0, SLJIT_R1, 0, SLJIT_IMM, 0x3F);
                sljit_emit_op2(compiler, SLJIT_SHL, SLJIT_R0, 0, SLJIT_R0, 0, SLJIT_R1, 0);
                ctx.pushLong(SLJIT_R0, 0);
                pc++;
                break;
            }

            case JavaInstruction::Lshr:
            {
                ctx.popInt(SLJIT_R1, 0);
                ctx.popLong(SLJIT_R0, 0);
                sljit_emit_op2(compiler, SLJIT_AND | SLJIT_32, SLJIT_R1, 0, SLJIT_R1, 0, SLJIT_IMM, 0x3F);
                sljit_emit_op2(compiler, SLJIT_ASHR, SLJIT_R0, 0, SLJIT_R0, 0, SLJIT_R1, 0);
                ctx.pushLong(SLJIT_R0, 0);
                pc++;
                break;
            }

            case JavaInstruction::Lushr:
            {
                ctx.popInt(SLJIT_R1, 0);
                ctx.popLong(SLJIT_R0, 0);
                sljit_emit_op2(compiler, SLJIT_AND | SLJIT_32, SLJIT_R1, 0, SLJIT_R1, 0, SLJIT_IMM, 0x3F);
                sljit_emit_op2(compiler, SLJIT_LSHR, SLJIT_R0, 0, SLJIT_R0, 0, SLJIT_R1, 0);
                ctx.pushLong(SLJIT_R0, 0);
                pc++;
                break;
            }

            case JavaInstruction::Land:
            {
                ctx.popLong(SLJIT_R1, 0);
                ctx.popLong(SLJIT_R0, 0);
                sljit_emit_op2(compiler, SLJIT_AND, SLJIT_R0, 0, SLJIT_R0, 0, SLJIT_R1, 0);
                ctx.pushLong(SLJIT_R0, 0);
                pc++;
                break;
            }

            case JavaInstruction::Lor:
            {
                ctx.popLong(SLJIT_R1, 0);
                ctx.popLong(SLJIT_R0, 0);
                sljit_emit_op2(compiler, SLJIT_OR, SLJIT_R0, 0, SLJIT_R0, 0, SLJIT_R1, 0);
                ctx.pushLong(SLJIT_R0, 0);
                pc++;
                break;
            }

            case JavaInstruction::Lxor:
            {
                ctx.popLong(SLJIT_R1, 0);
                ctx.popLong(SLJIT_R0, 0);
                sljit_emit_op2(compiler, SLJIT_XOR, SLJIT_R0, 0, SLJIT_R0, 0, SLJIT_R1, 0);
                ctx.pushLong(SLJIT_R0, 0);
                pc++;
                break;
            }

            case JavaInstruction::I2l:
            {
                ctx.popInt(SLJIT_R0, 0);
                sljit_emit_op1(compiler, SLJIT_MOV_S32, SLJIT_R0, 0, SLJIT_R0, 0);
                ctx.pushLong(SLJIT_R0, 0);
                pc++;
                break;
            }

            case JavaInstruction::L2i:
            {
                ctx.popLong(SLJIT_R0, 0);
                sljit_emit_op1(compiler, SLJIT_MOV32, SLJIT_R0, 0, SLJIT_R0, 0);
                ctx.pushInt(SLJIT_R0, 0);
                pc++;
                break;
            }

            case JavaInstruction::L2d:
            {
                ctx.popLong(SLJIT_R0, 0);
                sljit_emit_fop1(compiler, SLJIT_CONV_F64_FROM_SW, SLJIT_FR0, 0, SLJIT_R0, 0);
                ctx.pushDouble(SLJIT_FR0, 0);
                pc++;
                break;
            }

            case JavaInstruction::D2l:
            {
                ctx.popDouble(SLJIT_FR0, 0);
                sljit_emit_fop1(compiler, SLJIT_CONV_SW_FROM_F64, SLJIT_R0, 0, SLJIT_FR0, 0);
                ctx.pushLong(SLJIT_R0, 0);
                pc++;
                break;
            }

            case JavaInstruction::D2i:
            {
                ctx.popDouble(SLJIT_FR0, 0);
                sljit_emit_fop1(compiler, SLJIT_CONV_S32_FROM_F64, SLJIT_R0, 0, SLJIT_FR0, 0);
                ctx.pushInt(SLJIT_R0, 0);
                pc++;
                break;
            }

            case JavaInstruction::I2d:
            {
                ctx.popInt(SLJIT_R0, 0);
                sljit_emit_op1(compiler, SLJIT_MOV_S32, SLJIT_R0, 0, SLJIT_R0, 0);
                sljit_emit_fop1(compiler, SLJIT_CONV_F64_FROM_SW, SLJIT_FR0, 0, SLJIT_R0, 0);
                ctx.pushDouble(SLJIT_FR0, 0);
                pc++;
                break;
            }

            case JavaInstruction::I2b:
            {
                ctx.popInt(SLJIT_R0, 0);
                sljit_emit_op1(compiler, SLJIT_MOV_S8, SLJIT_R0, 0, SLJIT_R0, 0);
                ctx.pushInt(SLJIT_R0, 0);
                pc++;
                break;
            }

            case JavaInstruction::I2c:
            {
                ctx.popInt(SLJIT_R0, 0);
                sljit_emit_op2(compiler, SLJIT_AND | SLJIT_32, SLJIT_R0, 0, SLJIT_R0, 0, SLJIT_IMM, 0xFFFF);
                ctx.pushInt(SLJIT_R0, 0);
                pc++;
                break;
            }

            case JavaInstruction::I2s:
            {
                ctx.popInt(SLJIT_R0, 0);
                sljit_emit_op1(compiler, SLJIT_MOV_S16, SLJIT_R0, 0, SLJIT_R0, 0);
                ctx.pushInt(SLJIT_R0, 0);
                pc++;
                break;
            }

            case JavaInstruction::Iinc:
            {
                if (pc + 2 >= bc.size()) {
                    break;
                }
                int idx = bc[pc + 1];
                int8_t incr = (int8_t) bc[pc + 2];
                ctx.loadLocalInt(idx, SLJIT_R0, 0);
                sljit_emit_op2(compiler, SLJIT_ADD | SLJIT_32, SLJIT_R0, 0, SLJIT_R0, 0, SLJIT_IMM, incr);
                ctx.storeLocalInt(idx, SLJIT_R0, 0);
                pc += 3;
                break;
            }


            case JavaInstruction::LLoad_0:
            case JavaInstruction::LLoad_1:
            case JavaInstruction::LLoad_2:
            case JavaInstruction::LLoad_3:
            {
                int idx = static_cast<int> (inst) - static_cast<int> (JavaInstruction::LLoad_0);
                ctx.loadLocalLong(idx, SLJIT_R0, 0);
                ctx.pushLong(SLJIT_R0, 0);
                pc++;
                break;
            }

            case JavaInstruction::LStore_0:
            case JavaInstruction::LStore_1:
            case JavaInstruction::LStore_2:
            case JavaInstruction::LStore_3:
            {
                int idx = static_cast<int> (inst) - static_cast<int> (JavaInstruction::LStore_0);
                ctx.popLong(SLJIT_R0, 0);
                ctx.storeLocalLong(idx, SLJIT_R0, 0);
                pc++;
                break;
            }


            case JavaInstruction::ILoad_0:
            case JavaInstruction::ILoad_1:
            case JavaInstruction::ILoad_2:
            case JavaInstruction::ILoad_3:
            {
                int idx = static_cast<int> (inst) - static_cast<int> (JavaInstruction::ILoad_0);
                ctx.loadLocalInt(idx, SLJIT_R0, 0);
                ctx.pushInt(SLJIT_R0, 0);
                pc++;
                break;
            }

            case JavaInstruction::IStore_0:
            case JavaInstruction::IStore_1:
            case JavaInstruction::IStore_2:
            case JavaInstruction::IStore_3:
            {
                int idx = static_cast<int> (inst) - static_cast<int> (JavaInstruction::IStore_0);
                ctx.popInt(SLJIT_R0, 0);
                ctx.storeLocalInt(idx, SLJIT_R0, 0);
                pc++;
                break;
            }

            case JavaInstruction::Ladd:
            {
                ctx.popLong(SLJIT_R1, 0);
                ctx.popLong(SLJIT_R0, 0);
                sljit_emit_op2(compiler, SLJIT_ADD, SLJIT_R0, 0, SLJIT_R0, 0, SLJIT_R1, 0);
                ctx.pushLong(SLJIT_R0, 0);
                pc++;
                break;
            }

            case JavaInstruction::Lsub:
            {
                ctx.popLong(SLJIT_R1, 0);
                ctx.popLong(SLJIT_R0, 0);
                sljit_emit_op2(compiler, SLJIT_SUB, SLJIT_R0, 0, SLJIT_R0, 0, SLJIT_R1, 0);
                ctx.pushLong(SLJIT_R0, 0);
                pc++;
                break;
            }

            case JavaInstruction::Lmul:
            {
                ctx.popLong(SLJIT_R1, 0);
                ctx.popLong(SLJIT_R0, 0);
                sljit_emit_op2(compiler, SLJIT_MUL, SLJIT_R0, 0, SLJIT_R0, 0, SLJIT_R1, 0);
                ctx.pushLong(SLJIT_R0, 0);
                pc++;
                break;
            }

            case JavaInstruction::Ldiv:
            {
                ctx.popLong(SLJIT_R1, 0);
                ctx.popLong(SLJIT_R0, 0);
                sljit_emit_op0(compiler, SLJIT_DIVMOD_SW);
                ctx.pushLong(SLJIT_R0, 0);
                pc++;
                break;
            }

            case JavaInstruction::Lrem:
            {
                ctx.popLong(SLJIT_R1, 0); // делитель
                ctx.popLong(SLJIT_R0, 0); // делимое

                // КРИТИЧНО: Проверка на 0
                auto jump_zero = sljit_emit_cmp(compiler, SLJIT_EQUAL,
                        SLJIT_R1, 0, SLJIT_IMM, 0);

                // КРИТИЧНО: Проверка на INT64_MIN / -1 (overflow!)
                sljit_emit_op2(compiler, SLJIT_XOR, SLJIT_R2, 0,
                        SLJIT_R1, 0, SLJIT_IMM, -1);
                auto jump_neg1 = sljit_emit_cmp(compiler, SLJIT_NOT_EQUAL,
                        SLJIT_R2, 0, SLJIT_IMM, 0);

                sljit_emit_op2(compiler, SLJIT_XOR, SLJIT_R2, 0,
                        SLJIT_R0, 0, SLJIT_IMM, INT64_MIN);
                auto jump_not_min = sljit_emit_cmp(compiler, SLJIT_NOT_EQUAL,
                        SLJIT_R2, 0, SLJIT_IMM, 0);

                // Если R0 == INT64_MIN и R1 == -1, результат = 0
                sljit_emit_op1(compiler, SLJIT_MOV, SLJIT_R1, 0, SLJIT_IMM, 0);
                auto jump_end_overflow = sljit_emit_jump(compiler, SLJIT_JUMP);

                // Нормальный случай
                sljit_set_label(jump_neg1, sljit_emit_label(compiler));
                sljit_set_label(jump_not_min, sljit_emit_label(compiler));

                sljit_emit_op0(compiler, SLJIT_DIVMOD_SW);
                auto jump_end = sljit_emit_jump(compiler, SLJIT_JUMP);

                // Деление на 0
                sljit_set_label(jump_zero, sljit_emit_label(compiler));
                sljit_emit_op1(compiler, SLJIT_MOV, SLJIT_R1, 0, SLJIT_IMM, 0);

                sljit_set_label(jump_end, sljit_emit_label(compiler));
                sljit_set_label(jump_end_overflow, sljit_emit_label(compiler));

                ctx.pushLong(SLJIT_R1, 0);
                pc++;
                break;
            }

            case JavaInstruction::Iadd:
            {
                ctx.popInt(SLJIT_R1, 0);
                ctx.popInt(SLJIT_R0, 0);
                sljit_emit_op2(compiler, SLJIT_ADD | SLJIT_32, SLJIT_R0, 0, SLJIT_R0, 0, SLJIT_R1, 0);
                ctx.pushInt(SLJIT_R0, 0);
                pc++;
                break;
            }

            case JavaInstruction::Isub:
            {
                ctx.popInt(SLJIT_R1, 0);
                ctx.popInt(SLJIT_R0, 0);
                sljit_emit_op2(compiler, SLJIT_SUB | SLJIT_32, SLJIT_R0, 0, SLJIT_R0, 0, SLJIT_R1, 0);
                ctx.pushInt(SLJIT_R0, 0);
                pc++;
                break;
            }

            case JavaInstruction::Imul:
            {
                ctx.popInt(SLJIT_R1, 0);
                ctx.popInt(SLJIT_R0, 0);
                sljit_emit_op2(compiler, SLJIT_MUL | SLJIT_32, SLJIT_R0, 0, SLJIT_R0, 0, SLJIT_R1, 0);
                ctx.pushInt(SLJIT_R0, 0);
                pc++;
                break;
            }

            case JavaInstruction::Idiv:
            {
                ctx.popInt(SLJIT_R1, 0);
                ctx.popInt(SLJIT_R0, 0);
                sljit_emit_op0(compiler, SLJIT_DIVMOD_S32);
                ctx.pushInt(SLJIT_R0, 0);
                pc++;
                break;
            }

            case JavaInstruction::Irem:
            {
                ctx.popInt(SLJIT_R1, 0); // делитель
                ctx.popInt(SLJIT_R0, 0); // делимое

                // КРИТИЧНО: Проверяем деление на 0
                auto jump_zero = sljit_emit_cmp(compiler, SLJIT_EQUAL, SLJIT_R1, 0, SLJIT_IMM, 0);

                // Нормальный случай
                sljit_emit_op0(compiler, SLJIT_DIVMOD_S32);
                auto jump_end = sljit_emit_jump(compiler, SLJIT_JUMP);

                // Деление на 0: Java бросает ArithmeticException
                // Для простоты возвращаем 0 или вызываем abort
                sljit_set_label(jump_zero, sljit_emit_label(compiler));
                sljit_emit_op1(compiler, SLJIT_MOV32, SLJIT_R1, 0, SLJIT_IMM, 0);

                sljit_set_label(jump_end, sljit_emit_label(compiler));
                ctx.pushInt(SLJIT_R1, 0); // остаток в R1
                pc++;
                break;
            }

            case JavaInstruction::LConst_0:
            {
                ctx.pushLong(SLJIT_IMM, 0);
                pc++;
                break;
            }

            case JavaInstruction::LConst_1:
            {
                ctx.pushLong(SLJIT_IMM, 1);
                pc++;
                break;
            }

            case JavaInstruction::Lneg:
            {
                ctx.popLong(SLJIT_R0, 0);
                sljit_emit_op2(compiler, SLJIT_SUB, SLJIT_R0, 0, SLJIT_IMM, 0, SLJIT_R0, 0);
                ctx.pushLong(SLJIT_R0, 0);
                pc++;
                break;
            }

            case JavaInstruction::IConst_m1:
            {
                ctx.pushInt(SLJIT_IMM, -1);
                pc++;
                break;
            }

            case JavaInstruction::Ineg:
            {
                ctx.popInt(SLJIT_R0, 0);
                sljit_emit_op2(compiler, SLJIT_SUB, SLJIT_R0, 0, SLJIT_IMM, 0, SLJIT_R0, 0);
                ctx.pushInt(SLJIT_R0, 0);
                pc++;
                break;
            }

            case JavaInstruction::SiPush:
            {
                if (pc + 2 >= bc.size()) {
                    break;
                }
                int16_t value = (int16_t) ((bc[pc + 1] << 8) | bc[pc + 2]);
                ctx.pushInt(SLJIT_IMM, value);
                pc += 3;
                break;
            }

            case JavaInstruction::IReturn:
            {
                ctx.popInt(SLJIT_R0, 0);
                sljit_emit_return(compiler, SLJIT_MOV_S32, SLJIT_R0, 0);
                pc++;
                break;
            }

            case JavaInstruction::L2f:
            {
                ctx.popLong(SLJIT_R0, 0);
                sljit_emit_fop1(compiler, SLJIT_CONV_F32_FROM_SW, SLJIT_FR0, 0, SLJIT_R0, 0);
                sljit_emit_fop1(compiler, SLJIT_MOV_F32, SLJIT_MEM1(SLJIT_SP), ctx.stackBase + ctx.stackPtr * sizeof (sljit_sw), SLJIT_FR0, 0);
                ctx.stackPtr++;
                pc++;
                break;
            }

            case JavaInstruction::I2f:
            {
                ctx.popInt(SLJIT_R0, 0);
                sljit_emit_fop1(compiler, SLJIT_CONV_F32_FROM_SW, SLJIT_FR0, 0, SLJIT_R0, 0);
                sljit_emit_fop1(compiler, SLJIT_MOV_F32, SLJIT_MEM1(SLJIT_SP), ctx.stackBase + ctx.stackPtr * sizeof (sljit_sw), SLJIT_FR0, 0);
                ctx.stackPtr++;
                pc++;
                break;
            }

            case JavaInstruction::F2i:
            {
                ctx.stackPtr--;
                sljit_emit_fop1(compiler, SLJIT_MOV_F32, SLJIT_FR0, 0, SLJIT_MEM1(SLJIT_SP), ctx.stackBase + ctx.stackPtr * sizeof (sljit_sw));
                sljit_emit_fop1(compiler, SLJIT_CONV_SW_FROM_F32, SLJIT_R0, 0, SLJIT_FR0, 0);
                ctx.pushInt(SLJIT_R0, 0);
                pc++;
                break;
            }

            case JavaInstruction::F2l:
            {
                ctx.stackPtr--;
                sljit_emit_fop1(compiler, SLJIT_MOV_F32, SLJIT_FR0, 0, SLJIT_MEM1(SLJIT_SP), ctx.stackBase + ctx.stackPtr * sizeof (sljit_sw));
                sljit_emit_fop1(compiler, SLJIT_CONV_SW_FROM_F32, SLJIT_R0, 0, SLJIT_FR0, 0);
                ctx.pushLong(SLJIT_R0, 0);
                pc++;
                break;
            }

            case JavaInstruction::F2d:
            {
                ctx.stackPtr--;
                sljit_emit_fop1(compiler, SLJIT_MOV_F32, SLJIT_FR0, 0, SLJIT_MEM1(SLJIT_SP), ctx.stackBase + ctx.stackPtr * sizeof (sljit_sw));
                sljit_emit_fop1(compiler, SLJIT_CONV_F64_FROM_F32, SLJIT_FR0, 0, SLJIT_FR0, 0);
                ctx.pushDouble(SLJIT_FR0, 0);
                pc++;
                break;
            }

            case JavaInstruction::D2f:
            {
                ctx.popDouble(SLJIT_FR0, 0);
                sljit_emit_fop1(compiler, SLJIT_CONV_F32_FROM_F64, SLJIT_FR0, 0, SLJIT_FR0, 0);
                sljit_emit_fop1(compiler, SLJIT_MOV_F32, SLJIT_MEM1(SLJIT_SP), ctx.stackBase + ctx.stackPtr * sizeof (sljit_sw), SLJIT_FR0, 0);
                ctx.stackPtr++;
                pc++;
                break;
            }

            case JavaInstruction::DLoad_0:
            case JavaInstruction::DLoad_1:
            case JavaInstruction::DLoad_3:
            {
                int idx = static_cast<int> (inst) - static_cast<int> (JavaInstruction::DLoad_0);
                ctx.loadLocalDouble(idx, SLJIT_FR0, 0);
                ctx.pushDouble(SLJIT_FR0, 0);
                pc++;
                break;
            }

            case JavaInstruction::DStore_0:
            case JavaInstruction::DStore_1:
            case JavaInstruction::DStore_3:
            {
                int idx = static_cast<int> (inst) - static_cast<int> (JavaInstruction::DStore_0);
                ctx.popDouble(SLJIT_FR0, 0);
                ctx.storeLocalDouble(idx, SLJIT_FR0, 0);
                pc++;
                break;
            }

            case JavaInstruction::Dneg:
            {
                ctx.popDouble(SLJIT_FR0, 0);
                sljit_emit_fop1(compiler, SLJIT_NEG_F64, SLJIT_FR0, 0, SLJIT_FR0, 0);
                ctx.pushDouble(SLJIT_FR0, 0);
                pc++;
                break;
            }

            case JavaInstruction::FConst_0:
            {
                sljit_emit_fset32(compiler, SLJIT_FR0, 0.0f);
                sljit_emit_fop1(compiler, SLJIT_MOV_F32, SLJIT_MEM1(SLJIT_SP), ctx.stackBase + ctx.stackPtr * sizeof (sljit_sw), SLJIT_FR0, 0);
                ctx.stackPtr++;
                pc++;
                break;
            }

            case JavaInstruction::FConst_1:
            {
                sljit_emit_fset32(compiler, SLJIT_FR0, 1.0f);
                sljit_emit_fop1(compiler, SLJIT_MOV_F32, SLJIT_MEM1(SLJIT_SP), ctx.stackBase + ctx.stackPtr * sizeof (sljit_sw), SLJIT_FR0, 0);
                ctx.stackPtr++;
                pc++;
                break;
            }

            case JavaInstruction::FConst_2:
            {
                sljit_emit_fset32(compiler, SLJIT_FR0, 2.0f);
                sljit_emit_fop1(compiler, SLJIT_MOV_F32, SLJIT_MEM1(SLJIT_SP), ctx.stackBase + ctx.stackPtr * sizeof (sljit_sw), SLJIT_FR0, 0);
                ctx.stackPtr++;
                pc++;
                break;
            }

            case JavaInstruction::FLoad:
            {
                if (pc + 1 >= bc.size()) {
                    break;
                }
                int idx = bc[pc + 1];
                sljit_emit_fop1(compiler, SLJIT_MOV_F32, SLJIT_FR0, 0, SLJIT_MEM1(SLJIT_SP), idx * sizeof (sljit_sw));
                sljit_emit_fop1(compiler, SLJIT_MOV_F32, SLJIT_MEM1(SLJIT_SP), ctx.stackBase + ctx.stackPtr * sizeof (sljit_sw), SLJIT_FR0, 0);
                ctx.stackPtr++;
                pc += 2;
                break;
            }

            case JavaInstruction::FLoad_0:
            case JavaInstruction::FLoad_1:
            case JavaInstruction::FLoad_2:
            case JavaInstruction::FLoad_3:
            {
                int idx = static_cast<int> (inst) - static_cast<int> (JavaInstruction::FLoad_0);
                sljit_emit_fop1(compiler, SLJIT_MOV_F32, SLJIT_FR0, 0, SLJIT_MEM1(SLJIT_SP), idx * sizeof (sljit_sw));
                sljit_emit_fop1(compiler, SLJIT_MOV_F32, SLJIT_MEM1(SLJIT_SP), ctx.stackBase + ctx.stackPtr * sizeof (sljit_sw), SLJIT_FR0, 0);
                ctx.stackPtr++;
                pc++;
                break;
            }

            case JavaInstruction::FStore:
            {
                if (pc + 1 >= bc.size()) {
                    break;
                }
                int idx = bc[pc + 1];
                ctx.stackPtr--;
                sljit_emit_fop1(compiler, SLJIT_MOV_F32, SLJIT_FR0, 0, SLJIT_MEM1(SLJIT_SP), ctx.stackBase + ctx.stackPtr * sizeof (sljit_sw));
                sljit_emit_fop1(compiler, SLJIT_MOV_F32, SLJIT_MEM1(SLJIT_SP), idx * sizeof (sljit_sw), SLJIT_FR0, 0);
                pc += 2;
                break;
            }

            case JavaInstruction::FStore_0:
            case JavaInstruction::FStore_1:
            case JavaInstruction::FStore_2:
            case JavaInstruction::FStore_3:
            {
                int idx = static_cast<int> (inst) - static_cast<int> (JavaInstruction::FStore_0);
                ctx.stackPtr--;
                sljit_emit_fop1(compiler, SLJIT_MOV_F32, SLJIT_FR0, 0, SLJIT_MEM1(SLJIT_SP), ctx.stackBase + ctx.stackPtr * sizeof (sljit_sw));
                sljit_emit_fop1(compiler, SLJIT_MOV_F32, SLJIT_MEM1(SLJIT_SP), idx * sizeof (sljit_sw), SLJIT_FR0, 0);
                pc++;
                break;
            }

            case JavaInstruction::Fadd:
            {
                ctx.stackPtr--;
                sljit_emit_fop1(compiler, SLJIT_MOV_F32, SLJIT_FR1, 0, SLJIT_MEM1(SLJIT_SP), ctx.stackBase + ctx.stackPtr * sizeof (sljit_sw));
                ctx.stackPtr--;
                sljit_emit_fop1(compiler, SLJIT_MOV_F32, SLJIT_FR0, 0, SLJIT_MEM1(SLJIT_SP), ctx.stackBase + ctx.stackPtr * sizeof (sljit_sw));
                sljit_emit_fop2(compiler, SLJIT_ADD_F32, SLJIT_FR0, 0, SLJIT_FR0, 0, SLJIT_FR1, 0);
                sljit_emit_fop1(compiler, SLJIT_MOV_F32, SLJIT_MEM1(SLJIT_SP), ctx.stackBase + ctx.stackPtr * sizeof (sljit_sw), SLJIT_FR0, 0);
                ctx.stackPtr++;
                pc++;
                break;
            }

            case JavaInstruction::Fsub:
            {
                ctx.stackPtr--;
                sljit_emit_fop1(compiler, SLJIT_MOV_F32, SLJIT_FR1, 0, SLJIT_MEM1(SLJIT_SP), ctx.stackBase + ctx.stackPtr * sizeof (sljit_sw));
                ctx.stackPtr--;
                sljit_emit_fop1(compiler, SLJIT_MOV_F32, SLJIT_FR0, 0, SLJIT_MEM1(SLJIT_SP), ctx.stackBase + ctx.stackPtr * sizeof (sljit_sw));
                sljit_emit_fop2(compiler, SLJIT_SUB_F32, SLJIT_FR0, 0, SLJIT_FR0, 0, SLJIT_FR1, 0);
                sljit_emit_fop1(compiler, SLJIT_MOV_F32, SLJIT_MEM1(SLJIT_SP), ctx.stackBase + ctx.stackPtr * sizeof (sljit_sw), SLJIT_FR0, 0);
                ctx.stackPtr++;
                pc++;
                break;
            }

            case JavaInstruction::Fmul:
            {
                ctx.stackPtr--;
                sljit_emit_fop1(compiler, SLJIT_MOV_F32, SLJIT_FR1, 0, SLJIT_MEM1(SLJIT_SP), ctx.stackBase + ctx.stackPtr * sizeof (sljit_sw));
                ctx.stackPtr--;
                sljit_emit_fop1(compiler, SLJIT_MOV_F32, SLJIT_FR0, 0, SLJIT_MEM1(SLJIT_SP), ctx.stackBase + ctx.stackPtr * sizeof (sljit_sw));
                sljit_emit_fop2(compiler, SLJIT_MUL_F32, SLJIT_FR0, 0, SLJIT_FR0, 0, SLJIT_FR1, 0);
                sljit_emit_fop1(compiler, SLJIT_MOV_F32, SLJIT_MEM1(SLJIT_SP), ctx.stackBase + ctx.stackPtr * sizeof (sljit_sw), SLJIT_FR0, 0);
                ctx.stackPtr++;
                pc++;
                break;
            }

            case JavaInstruction::Fdiv:
            {
                ctx.stackPtr--;
                sljit_emit_fop1(compiler, SLJIT_MOV_F32, SLJIT_FR1, 0, SLJIT_MEM1(SLJIT_SP), ctx.stackBase + ctx.stackPtr * sizeof (sljit_sw));
                ctx.stackPtr--;
                sljit_emit_fop1(compiler, SLJIT_MOV_F32, SLJIT_FR0, 0, SLJIT_MEM1(SLJIT_SP), ctx.stackBase + ctx.stackPtr * sizeof (sljit_sw));
                sljit_emit_fop2(compiler, SLJIT_DIV_F32, SLJIT_FR0, 0, SLJIT_FR0, 0, SLJIT_FR1, 0);
                sljit_emit_fop1(compiler, SLJIT_MOV_F32, SLJIT_MEM1(SLJIT_SP), ctx.stackBase + ctx.stackPtr * sizeof (sljit_sw), SLJIT_FR0, 0);
                ctx.stackPtr++;
                pc++;
                break;
            }

            case JavaInstruction::Frem:
            {
                ctx.stackPtr--;
                sljit_emit_fop1(compiler, SLJIT_MOV_F32, SLJIT_FR1, 0, SLJIT_MEM1(SLJIT_SP), ctx.stackBase + ctx.stackPtr * sizeof (sljit_sw));
                ctx.stackPtr--;
                sljit_emit_fop1(compiler, SLJIT_MOV_F32, SLJIT_FR0, 0, SLJIT_MEM1(SLJIT_SP), ctx.stackBase + ctx.stackPtr * sizeof (sljit_sw));
                sljit_emit_fop2(compiler, SLJIT_DIV_F32, SLJIT_FR2, 0, SLJIT_FR0, 0, SLJIT_FR1, 0);
                sljit_emit_fop1(compiler, SLJIT_CONV_S32_FROM_F32, SLJIT_R0, 0, SLJIT_FR2, 0);
                sljit_emit_fop1(compiler, SLJIT_CONV_F32_FROM_S32, SLJIT_FR2, 0, SLJIT_R0, 0);
                sljit_emit_fop2(compiler, SLJIT_MUL_F32, SLJIT_FR2, 0, SLJIT_FR2, 0, SLJIT_FR1, 0);
                sljit_emit_fop2(compiler, SLJIT_SUB_F32, SLJIT_FR0, 0, SLJIT_FR0, 0, SLJIT_FR2, 0);
                sljit_emit_fop1(compiler, SLJIT_MOV_F32, SLJIT_MEM1(SLJIT_SP), ctx.stackBase + ctx.stackPtr * sizeof (sljit_sw), SLJIT_FR0, 0);
                ctx.stackPtr++;
                pc++;
                break;
            }

            case JavaInstruction::Fneg:
            {
                ctx.stackPtr--;
                sljit_emit_fop1(compiler, SLJIT_MOV_F32, SLJIT_FR0, 0, SLJIT_MEM1(SLJIT_SP), ctx.stackBase + ctx.stackPtr * sizeof (sljit_sw));
                sljit_emit_fop1(compiler, SLJIT_NEG_F32, SLJIT_FR0, 0, SLJIT_FR0, 0);
                sljit_emit_fop1(compiler, SLJIT_MOV_F32, SLJIT_MEM1(SLJIT_SP), ctx.stackBase + ctx.stackPtr * sizeof (sljit_sw), SLJIT_FR0, 0);
                ctx.stackPtr++;
                pc++;
                break;
            }

            case JavaInstruction::FReturn:
            {
                ctx.stackPtr--;
                sljit_emit_fop1(compiler, SLJIT_MOV_F32, SLJIT_FR0, 0, SLJIT_MEM1(SLJIT_SP), ctx.stackBase + ctx.stackPtr * sizeof (sljit_sw));
                sljit_emit_return(compiler, SLJIT_MOV_F32, SLJIT_FR0, 0);
                pc++;
                break;
            }
            case JavaInstruction::Pop:
            {
                ctx.stackPtr--;
                pc++;
                break;
            }

            case JavaInstruction::Pop2:
            {
                ctx.stackPtr -= 2;
                pc++;
                break;
            }

            case JavaInstruction::Dup:
            {
                sljit_emit_op1(compiler, SLJIT_MOV, SLJIT_R0, 0, SLJIT_MEM1(SLJIT_SP), ctx.stackBase + (ctx.stackPtr - 1) * sizeof (sljit_sw));
                sljit_emit_op1(compiler, SLJIT_MOV, SLJIT_MEM1(SLJIT_SP), ctx.stackBase + ctx.stackPtr * sizeof (sljit_sw), SLJIT_R0, 0);
                ctx.stackPtr++;
                pc++;
                break;
            }

            case JavaInstruction::Dup2:
            {
                sljit_emit_op1(compiler, SLJIT_MOV, SLJIT_R0, 0, SLJIT_MEM1(SLJIT_SP), ctx.stackBase + (ctx.stackPtr - 2) * sizeof (sljit_sw));
                sljit_emit_op1(compiler, SLJIT_MOV, SLJIT_R1, 0, SLJIT_MEM1(SLJIT_SP), ctx.stackBase + (ctx.stackPtr - 1) * sizeof (sljit_sw));
                sljit_emit_op1(compiler, SLJIT_MOV, SLJIT_MEM1(SLJIT_SP), ctx.stackBase + ctx.stackPtr * sizeof (sljit_sw), SLJIT_R0, 0);
                sljit_emit_op1(compiler, SLJIT_MOV, SLJIT_MEM1(SLJIT_SP), ctx.stackBase + (ctx.stackPtr + 1) * sizeof (sljit_sw), SLJIT_R1, 0);
                ctx.stackPtr += 2;
                pc++;
                break;
            }

            case JavaInstruction::Dup_x1:
            {
                sljit_emit_op1(compiler, SLJIT_MOV, SLJIT_R0, 0, SLJIT_MEM1(SLJIT_SP), ctx.stackBase + (ctx.stackPtr - 1) * sizeof (sljit_sw));
                sljit_emit_op1(compiler, SLJIT_MOV, SLJIT_R1, 0, SLJIT_MEM1(SLJIT_SP), ctx.stackBase + (ctx.stackPtr - 2) * sizeof (sljit_sw));
                sljit_emit_op1(compiler, SLJIT_MOV, SLJIT_MEM1(SLJIT_SP), ctx.stackBase + (ctx.stackPtr - 2) * sizeof (sljit_sw), SLJIT_R0, 0);
                sljit_emit_op1(compiler, SLJIT_MOV, SLJIT_MEM1(SLJIT_SP), ctx.stackBase + (ctx.stackPtr - 1) * sizeof (sljit_sw), SLJIT_R1, 0);
                sljit_emit_op1(compiler, SLJIT_MOV, SLJIT_MEM1(SLJIT_SP), ctx.stackBase + ctx.stackPtr * sizeof (sljit_sw), SLJIT_R0, 0);
                ctx.stackPtr++;
                pc++;
                break;
            }

            case JavaInstruction::Dup_x2:
            {
                sljit_emit_op1(compiler, SLJIT_MOV, SLJIT_R0, 0, SLJIT_MEM1(SLJIT_SP), ctx.stackBase + (ctx.stackPtr - 1) * sizeof (sljit_sw));
                sljit_emit_op1(compiler, SLJIT_MOV, SLJIT_R1, 0, SLJIT_MEM1(SLJIT_SP), ctx.stackBase + (ctx.stackPtr - 2) * sizeof (sljit_sw));
                sljit_emit_op1(compiler, SLJIT_MOV, SLJIT_R2, 0, SLJIT_MEM1(SLJIT_SP), ctx.stackBase + (ctx.stackPtr - 3) * sizeof (sljit_sw));
                sljit_emit_op1(compiler, SLJIT_MOV, SLJIT_MEM1(SLJIT_SP), ctx.stackBase + (ctx.stackPtr - 3) * sizeof (sljit_sw), SLJIT_R0, 0);
                sljit_emit_op1(compiler, SLJIT_MOV, SLJIT_MEM1(SLJIT_SP), ctx.stackBase + (ctx.stackPtr - 2) * sizeof (sljit_sw), SLJIT_R2, 0);
                sljit_emit_op1(compiler, SLJIT_MOV, SLJIT_MEM1(SLJIT_SP), ctx.stackBase + (ctx.stackPtr - 1) * sizeof (sljit_sw), SLJIT_R1, 0);
                sljit_emit_op1(compiler, SLJIT_MOV, SLJIT_MEM1(SLJIT_SP), ctx.stackBase + ctx.stackPtr * sizeof (sljit_sw), SLJIT_R0, 0);
                ctx.stackPtr++;
                pc++;
                break;
            }

            case JavaInstruction::Dup2_x1:
            {
                sljit_emit_op1(compiler, SLJIT_MOV, SLJIT_R0, 0, SLJIT_MEM1(SLJIT_SP), ctx.stackBase + (ctx.stackPtr - 1) * sizeof (sljit_sw));
                sljit_emit_op1(compiler, SLJIT_MOV, SLJIT_R1, 0, SLJIT_MEM1(SLJIT_SP), ctx.stackBase + (ctx.stackPtr - 2) * sizeof (sljit_sw));
                sljit_emit_op1(compiler, SLJIT_MOV, SLJIT_R2, 0, SLJIT_MEM1(SLJIT_SP), ctx.stackBase + (ctx.stackPtr - 3) * sizeof (sljit_sw));
                sljit_emit_op1(compiler, SLJIT_MOV, SLJIT_MEM1(SLJIT_SP), ctx.stackBase + (ctx.stackPtr - 3) * sizeof (sljit_sw), SLJIT_R1, 0);
                sljit_emit_op1(compiler, SLJIT_MOV, SLJIT_MEM1(SLJIT_SP), ctx.stackBase + (ctx.stackPtr - 2) * sizeof (sljit_sw), SLJIT_R0, 0);
                sljit_emit_op1(compiler, SLJIT_MOV, SLJIT_MEM1(SLJIT_SP), ctx.stackBase + (ctx.stackPtr - 1) * sizeof (sljit_sw), SLJIT_R2, 0);
                sljit_emit_op1(compiler, SLJIT_MOV, SLJIT_MEM1(SLJIT_SP), ctx.stackBase + ctx.stackPtr * sizeof (sljit_sw), SLJIT_R1, 0);
                sljit_emit_op1(compiler, SLJIT_MOV, SLJIT_MEM1(SLJIT_SP), ctx.stackBase + (ctx.stackPtr + 1) * sizeof (sljit_sw), SLJIT_R0, 0);
                ctx.stackPtr += 2;
                pc++;
                break;
            }

            case JavaInstruction::Dup2_x2:
            {
                sljit_emit_op1(compiler, SLJIT_MOV, SLJIT_R0, 0, SLJIT_MEM1(SLJIT_SP), ctx.stackBase + (ctx.stackPtr - 1) * sizeof (sljit_sw));
                sljit_emit_op1(compiler, SLJIT_MOV, SLJIT_R1, 0, SLJIT_MEM1(SLJIT_SP), ctx.stackBase + (ctx.stackPtr - 2) * sizeof (sljit_sw));
                sljit_emit_op1(compiler, SLJIT_MOV, SLJIT_R2, 0, SLJIT_MEM1(SLJIT_SP), ctx.stackBase + (ctx.stackPtr - 3) * sizeof (sljit_sw));
                sljit_emit_op1(compiler, SLJIT_MOV, SLJIT_R3, 0, SLJIT_MEM1(SLJIT_SP), ctx.stackBase + (ctx.stackPtr - 4) * sizeof (sljit_sw));
                sljit_emit_op1(compiler, SLJIT_MOV, SLJIT_MEM1(SLJIT_SP), ctx.stackBase + (ctx.stackPtr - 4) * sizeof (sljit_sw), SLJIT_R1, 0);
                sljit_emit_op1(compiler, SLJIT_MOV, SLJIT_MEM1(SLJIT_SP), ctx.stackBase + (ctx.stackPtr - 3) * sizeof (sljit_sw), SLJIT_R0, 0);
                sljit_emit_op1(compiler, SLJIT_MOV, SLJIT_MEM1(SLJIT_SP), ctx.stackBase + (ctx.stackPtr - 2) * sizeof (sljit_sw), SLJIT_R3, 0);
                sljit_emit_op1(compiler, SLJIT_MOV, SLJIT_MEM1(SLJIT_SP), ctx.stackBase + (ctx.stackPtr - 1) * sizeof (sljit_sw), SLJIT_R2, 0);
                sljit_emit_op1(compiler, SLJIT_MOV, SLJIT_MEM1(SLJIT_SP), ctx.stackBase + ctx.stackPtr * sizeof (sljit_sw), SLJIT_R1, 0);
                sljit_emit_op1(compiler, SLJIT_MOV, SLJIT_MEM1(SLJIT_SP), ctx.stackBase + (ctx.stackPtr + 1) * sizeof (sljit_sw), SLJIT_R0, 0);
                ctx.stackPtr += 2;
                pc++;
                break;
            }

            case JavaInstruction::Swap:
            {
                sljit_emit_op1(compiler, SLJIT_MOV, SLJIT_R0, 0, SLJIT_MEM1(SLJIT_SP), ctx.stackBase + (ctx.stackPtr - 1) * sizeof (sljit_sw));
                sljit_emit_op1(compiler, SLJIT_MOV, SLJIT_R1, 0, SLJIT_MEM1(SLJIT_SP), ctx.stackBase + (ctx.stackPtr - 2) * sizeof (sljit_sw));
                sljit_emit_op1(compiler, SLJIT_MOV, SLJIT_MEM1(SLJIT_SP), ctx.stackBase + (ctx.stackPtr - 2) * sizeof (sljit_sw), SLJIT_R0, 0);
                sljit_emit_op1(compiler, SLJIT_MOV, SLJIT_MEM1(SLJIT_SP), ctx.stackBase + (ctx.stackPtr - 1) * sizeof (sljit_sw), SLJIT_R1, 0);
                pc++;
                break;
            }

            case JavaInstruction::Return:
            {
                sljit_emit_return_void(compiler);
                pc++;
                break;
            }

            case JavaInstruction::DConst_0:
            {
                ctx.pushDoubleConst(0);
                pc++;
                break;
            }

            case JavaInstruction::DConst_1:
            {
                ctx.pushDoubleConst(1.0);
                pc++;
                break;
            }

            case JavaInstruction::DStore_2:
            {
                ctx.popDouble(SLJIT_FR0, 0);
                ctx.storeLocalDouble(2, SLJIT_FR0, 0);
                pc++;
                break;
            }

            case JavaInstruction::DStore:
            {
                if (pc + 1 >= bc.size()) {
                    break;
                }
                int idx = bc[pc + 1];
                ctx.popDouble(SLJIT_FR0, 0);
                ctx.storeLocalDouble(idx, SLJIT_FR0, 0);
                pc += 2;
                break;
            }

            case JavaInstruction::DLoad_2:
            {
                ctx.loadLocalDouble(2, SLJIT_FR0, 0);
                ctx.pushDouble(SLJIT_FR0, 0);
                pc++;
                break;
            }

            case JavaInstruction::DLoad:
            {
                if (pc + 1 >= bc.size()) {
                    break;
                }
                int idx = bc[pc + 1];
                ctx.loadLocalDouble(idx, SLJIT_FR0, 0);
                ctx.pushDouble(SLJIT_FR0, 0);
                pc += 2;
                break;
            }

            case JavaInstruction::Dmul:
            {
                ctx.popDouble(SLJIT_FR1, 0);
                ctx.popDouble(SLJIT_FR0, 0);
                sljit_emit_fop2(compiler, SLJIT_MUL_F64, SLJIT_FR0, 0, SLJIT_FR0, 0, SLJIT_FR1, 0);
                ctx.pushDouble(SLJIT_FR0, 0);
                pc++;
                break;
            }

            case JavaInstruction::Dadd:
            {
                ctx.popDouble(SLJIT_FR1, 0);
                ctx.popDouble(SLJIT_FR0, 0);
                sljit_emit_fop2(compiler, SLJIT_ADD_F64, SLJIT_FR0, 0, SLJIT_FR0, 0, SLJIT_FR1, 0);
                ctx.pushDouble(SLJIT_FR0, 0);
                pc++;
                break;
            }

            case JavaInstruction::Dsub:
            {
                ctx.popDouble(SLJIT_FR1, 0);
                ctx.popDouble(SLJIT_FR0, 0);
                sljit_emit_fop2(compiler, SLJIT_SUB_F64, SLJIT_FR0, 0, SLJIT_FR0, 0, SLJIT_FR1, 0);
                ctx.pushDouble(SLJIT_FR0, 0);
                pc++;
                break;
            }

            case JavaInstruction::Ddiv:
            {
                ctx.popDouble(SLJIT_FR1, 0);
                ctx.popDouble(SLJIT_FR0, 0);
                sljit_emit_fop2(compiler, SLJIT_DIV_F64, SLJIT_FR0, 0, SLJIT_FR0, 0, SLJIT_FR1, 0);
                ctx.pushDouble(SLJIT_FR0, 0);
                pc++;
                break;
            }

            case JavaInstruction::Drem:
            {
                ctx.popDouble(SLJIT_FR1, 0);
                ctx.popDouble(SLJIT_FR0, 0);
                sljit_emit_fop2(compiler, SLJIT_DIV_F64, SLJIT_FR2, 0, SLJIT_FR0, 0, SLJIT_FR1, 0);
                sljit_emit_fop1(compiler, SLJIT_CONV_SW_FROM_F64, SLJIT_R0, 0, SLJIT_FR2, 0);
                sljit_emit_fop1(compiler, SLJIT_CONV_F64_FROM_SW, SLJIT_FR2, 0, SLJIT_R0, 0);
                sljit_emit_fop2(compiler, SLJIT_MUL_F64, SLJIT_FR2, 0, SLJIT_FR2, 0, SLJIT_FR1, 0);
                sljit_emit_fop2(compiler, SLJIT_SUB_F64, SLJIT_FR0, 0, SLJIT_FR0, 0, SLJIT_FR2, 0);
                ctx.pushDouble(SLJIT_FR0, 0);
                pc++;
                break;
            }

            case JavaInstruction::DReturn:
            {
                ctx.popDouble(SLJIT_FR0, 0);
                sljit_emit_return(compiler, SLJIT_MOV_F64, SLJIT_FR0, 0);
                pc++;
                break;
            }


            case JavaInstruction::Ldc2_w:
            {
                if (pc + 2 >= bc.size()) {
                    break;
                }
                uint16_t idx = (bc[pc + 1] << 8) | bc[pc + 2];
                pc += 3;

                if (idx == 0 || idx >= clazz.pool.size()) {
                    std::cerr << "ldc2_w: invalid index " << idx << "\n";
                    ctx.pushDoubleConst(0.0);
                    break;
                }

                auto constant = clazz.pool[idx];
                auto tag = constant.get()->tag;

                if (tag == JavaConstantTag::Double) {
                    auto* dbl = JavaConstantDouble::cast(constant);
                    double val = dbl->value;
                    ctx.pushDoubleConst(val);
                    std::cout << "\t\tldc2_w Double #" << idx << " = " << val << "\n";
                } else if (tag == JavaConstantTag::Long) {
                    auto* lng = JavaConstantLong::cast(constant);
                    int64_t val = lng->value;
                    ctx.pushLong(SLJIT_IMM, val);
                    std::cout << "\t\tldc2_w Long #" << idx << " = " << val << "\n";
                } else {
                    std::cerr << "ldc2_w: unexpected tag " << (int) tag << "\n";
                    ctx.pushDoubleConst(0.0);
                }
                break;
            }

            case JavaInstruction::Ldc:
            case JavaInstruction::Ldc_w:
            {
                uint16_t idx;
                if (inst == JavaInstruction::Ldc) {
                    if (pc + 1 >= bc.size()) {
                        break;
                    }
                    idx = bc[++pc];
                    pc++;
                } else {
                    if (pc + 2 >= bc.size()) {
                        break;
                    }
                    idx = (bc[pc + 1] << 8) | bc[pc + 2];
                    pc += 3;
                }

                JavaConstant constant = clazz.pool[idx];

                switch (constant.get()->tag) {
                    case JavaConstantTag::Integer:
                    {
                        JavaConstantInteger* intConst = JavaConstantInteger::cast(constant);
                        int32_t value = intConst->value;
                        ctx.pushInt(SLJIT_IMM, value);
                        std::cout << "\t\tldc/ldc_w Integer #" << idx << " = " << value << "\n";
                        break;
                    }

                    case JavaConstantTag::Float:
                    {
                        JavaConstantFloat* floatConst = JavaConstantFloat::cast(constant);
                        float value = floatConst->value;

                        union {
                            float f;
                            uint32_t u;
                        } conv;
                        conv.f = value;
                        ctx.pushInt(SLJIT_IMM, (sljit_s32) conv.u);
                        std::cout << "\t\tldc/ldc_w Float #" << idx << " = " << value << "\n";
                        break;
                    }

                    case JavaConstantTag::Long:
                    {
                        JavaConstantLong* constVal = JavaConstantLong::cast(constant);
                        int64_t val = constVal->value;
                        ctx.pushLong(SLJIT_IMM, val);
                        std::cout << "\t\tldc/ldc_w Long #" << idx << " = " << val << "\n";
                        break;
                    }

                    case JavaConstantTag::Double:
                    {
                        JavaConstantDouble* constVal = JavaConstantDouble::cast(constant);
                        double val = constVal->value;
                        ctx.pushDoubleConst(val);
                        std::cout << "\t\tldc/ldc_w Double #" << idx << " = " << val << "\n";
                        break;
                    }

                    case JavaConstantTag::String:
                    case JavaConstantTag::Class:
                    {
                        ctx.pushInt(SLJIT_IMM, 0);
                        std::cout << "\t\tldc/ldc_w String/Class #" << idx << " → null (не реализовано)\n";
                        break;
                    }

                    default:
                    {
                        std::cerr << "ldc/ldc_w: неизвестный тип константы #" << idx << "\n";
                        ctx.pushInt(SLJIT_IMM, 0);
                        break;
                    }
                }
                break;
            }

            case JavaInstruction::IConst_0:
            case JavaInstruction::IConst_1:
            case JavaInstruction::IConst_2:
            case JavaInstruction::IConst_3:
            case JavaInstruction::IConst_4:
            case JavaInstruction::IConst_5:
            {
                ctx.pushInt(SLJIT_IMM, (int) inst - (int) JavaInstruction::IConst_0);
                pc++;
                break;
            }

            case JavaInstruction::BiPush:
            {
                ctx.pushInt(SLJIT_IMM, (int8_t) bc[pc + 1]);
                pc += 2;
                break;
            }

            case JavaInstruction::Goto:
            {
                if (pc + 2 >= bc.size()) {
                    break;
                }
                int16_t off = (int16_t) ((bc[pc + 1] << 8) | bc[pc + 2]);
                int target = pc + off;
                auto jump = sljit_emit_jump(compiler, SLJIT_JUMP);
                if (jump) {
                    ctx.addPendingJump(jump, target);
                } else {
                    cerr << "ERROR: Failed to create jump at pc " << pc << "\n";
                }
                pc += 3;
                break;
            }

            case JavaInstruction::LReturn:
            {
                ctx.popLong(SLJIT_R0, 0);
                sljit_emit_return(compiler, SLJIT_MOV, SLJIT_R0, 0);
                pc++;
                break;
            }

            default:
            {
                std::cerr << "Unimplemented instruction: " << info.name.c_str() << " (0x" << std::hex << (int) op << std::dec << ")\n";
                pc += info.length ? info.length : 1;
                break;
            }
        }
    }

    ctx.bindLabel(bc.size());
    ctx.resolvePendingJumps();

    cout << "=== Done ===\n";
}


//typedef double(*func2_t)(sljit_sw a, sljit_sw b);
typedef int64_t(*func2_t)(int32_t a, int32_t b);

int64_t source(int32_t n, int32_t m) {
    return (n * 2) << 2;
}

int main(int argc, char** argv) {
    struct sljit_compiler *compiler = nullptr;
    func2_t func = nullptr;
    void* code = nullptr;
    sljit_uw code_size = 0;
    compiler = sljit_create_compiler(NULL);
    if (!compiler) {
        fprintf(stderr, "Ошибка создания компилятора\n");
        return EXIT_FAILURE;
    }
    genCode(compiler);
    code = sljit_generate_code(compiler, 0, NULL);
    code_size = sljit_get_generated_code_size(compiler);
    sljit_free_compiler(compiler);
    if (!code) {
        fprintf(stderr, "Ошибка генерации кода\n");
        return EXIT_FAILURE;
    }
    dump_code_to_file(code, code_size, "jit_code.bin");
    func = (func2_t) code;
    printf("\n=== Тестирование функции ===\n");
    std::cout << "Result: " << std::to_string(func(1'000'000, 4)) << " source: " << std::to_string(source(4, 8)) << std::endl;
    for (int i = 0; i <= 5; i++) {
        auto start = std::chrono::high_resolution_clock::now();
        int64_t result = 0;
        for (int j = 0; j < 1; j++) {
            result = func(1'000'000, i);
        }
        auto end = std::chrono::high_resolution_clock::now();
        auto durationNanos = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
        auto durationMicros = durationNanos / 1'000;
        auto durationMillis = durationNanos / 1'000'000;
        double durationSeconds = durationNanos / 1'000'000'000.0;
        std::cout << "\n=====================================\n";
        std::cout << "Result: " << std::to_string(result) << "\n";
        //        std::cout << std::fixed << std::setprecision(17) << "Result: " << result << "\n";
        std::cout << "\tSeconds: " << durationSeconds << "\n";
        std::cout << "\tMillis: " << durationMillis << "\n";
        std::cout << "\tMicros: " << durationMicros << "\n";
        std::cout << "\tNanos: " << durationNanos << "\n";
    }
    sljit_free_code(code, NULL);
    return 0;
}
