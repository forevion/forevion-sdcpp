import { reactive } from "vue";

export interface ConfirmChoice {
    label: string;
    value: string;
    danger?: boolean;
    primary?: boolean;
}

export interface ConfirmRequest {
    title: string;
    message: string;
    details?: string[];
    warning?: string;
    choices: ConfirmChoice[];
}

// The open confirmation popup, rendered once by App.vue.
export const confirmState = reactive({
    request: null as ConfirmRequest | null,
    resolve: null as ((value: string | null) => void) | null,
});

// Shows a popup and resolves with the chosen value, or null when cancelled.
export function ask(request: ConfirmRequest): Promise<string | null> {
    confirmState.resolve?.(null);
    return new Promise((resolve) => {
        confirmState.request = request;
        confirmState.resolve = resolve;
    });
}

// Yes/no confirmation for a permanent action.
export async function confirmDanger(title: string, message: string, confirmLabel: string, details?: string[], warning?: string) {
    const answer = await ask({ title, message, details, warning, choices: [{ label: confirmLabel, value: "yes", danger: true }] });
    return answer === "yes";
}

export function answer(value: string | null) {
    const resolve = confirmState.resolve;
    confirmState.request = null;
    confirmState.resolve = null;
    resolve?.(value);
}
