# Design System Document

## 1. Overview & Creative North Star: "The Digital Vellum"

This design system is a manifesto for the e-ink medium. It rejects the frantic visual noise of backlit LCDs in favor of **The Digital Vellum**—a philosophy that treats the 480x800 canvas as a singular, high-end editorial sheet. 

Because we are working with a 1-bit constrained palette, we cannot rely on shadows or glows to convey importance. Instead, we move beyond "standard" UI by leveraging **intentional asymmetry**, **typographic weight**, and **whitespace as a structural element**. Every element must feel "printed" rather than "rendered." The goal is a tranquil, high-contrast experience that feels like a premium private library.

---

## 2. Colors: The Binary Palette

In this system, color is not a decorative choice but a functional one. We utilize a strict tonal hierarchy to manage the slow refresh rates of e-ink.

### The Palette
*   **Primary (#000000):** Reserved for the "Ink"—text, primary borders, and active states.
*   **Surface (#FFFFFF):** The "Paper"—our base canvas for maximum readability.
*   **Surface-Container (#EEEEEE):** The "Vellum"—used exclusively for selection highlights, secondary backgrounds, and subtle grouping.

### The "No-Line" Rule
Prohibit the use of 1px solid borders for sectioning content. Boundaries must be defined through **Whitespace (28px standard)** or **Background Tonal Shifts**. A list item does not sit between two lines; it sits within its own breathing room. If separation is mandatory, use a 12px vertical gap or a full-width background shift to `#EEEEEE`.

### Surface Hierarchy & Nesting
To create depth without shadows, we "stack" paper:
1.  **Level 0 (Surface):** The base app background.
2.  **Level 1 (Surface-Container):** Inset areas, such as a search field or a toggled menu item.
3.  **The High-Contrast Flip:** For high-priority CTAs, invert the stack (Primary background with Surface text).

---

## 3. Typography: Editorial Authority

Typography is the "architecture" of this system. We use a pairing of **Noto Serif** (for soul) and **Inter** (for utility).

| Level | Token | Font | Size | Weight | Intent |
| :--- | :--- | :--- | :--- | :--- | :--- |
| **Display** | `display-md` | Noto Serif | 2.75rem | Bold | Chapter starts, big moments. |
| **Headline** | `headline-sm` | Noto Serif | 1.5rem | Semi-Bold | Section headers. |
| **Title** | `title-md` | Noto Serif | 1.125rem | Medium | Book titles, primary navigation. |
| **Body** | `body-lg` | Noto Serif | 1rem | Regular | Long-form reading content. |
| **Label** | `label-md` | Inter | 0.75rem | Medium | UI Metadata, timestamps, buttons. |

**The Signature Layout:** To break the template feel, titles should often be set with generous top-padding (56px+) and left-aligned to the 28px margin, while body text may be slightly indented to create a sophisticated, asymmetrical "gutter" look reminiscent of modern typesetting.

---

## 4. Elevation & Depth: Tonal Layering

Traditional elevation (Z-axis) is impossible in 1-bit. We achieve "Digital Lift" through **The Layering Principle**.

*   **Tonal Stacking:** To highlight a card or modal, change the background from `surface` to `surface-container (#EEEEEE)`. 
*   **The Ghost Border Fallback:** If a component requires a defined edge (like a button), use a 1px border of `primary` (#000000). Never use gray borders; they appear dithered and "dirty" on e-ink.
*   **Inversion for Focus:** The most "elevated" element in this system is an inverted block. A black box on a white page creates a psychological "cut-out" that draws the eye immediately.

---

## 5. Components: Text-First Primitives

Components must be "primitive"—stripped of all unnecessary chrome to ensure zero-latency visual parsing.

### Buttons
*   **Primary:** Solid `primary` (#000000) background, `on-primary` (#FFFFFF) text. 6px corner radius (`md`).
*   **Secondary:** 1px `primary` border, `surface` background, `primary` text. 
*   **Tertiary:** Pure text (`label-md`) with an underline or simply extra padding. No container.

### Lists & Cards
*   **Forbid Divider Lines:** Separate list items with 16px of vertical whitespace. 
*   **The Highlight State:** On click/selection, the list item background shifts to `surface-container` (#EEEEEE). This provides a clear "ghost" effect that is easy on the screen's refresh cycle.

### Input Fields
*   No boxes. Use a single 1px bottom border (`primary`) with 8px of bottom padding. Labels use `label-sm` in Inter, positioned 4px above the line.

### Progress Indicators
*   Avoid spinning wheels (high ghosting). Use a simple "X of Y" text counter or a static horizontal bar that fills with solid black.

---

## 6. Do's and Don'ts

### Do:
*   **Respect the 28px Margin:** This is the "frame" that makes the UI feel premium.
*   **Use Asymmetry:** Place titles and body text on different alignments to create an editorial feel.
*   **Prioritize Contrast:** Ensure all text is 100% black on 100% white whenever possible.

### Don't:
*   **No Icons:** If you need a "Back" button, write the word "Back." If you need "Settings," write "Settings." 
*   **No Gradients/Shadows:** E-ink cannot render these without "banding" or "dithering," which destroys the premium aesthetic.
*   **No 1px Dividers:** They create visual clutter. Use space to define boundaries.
*   **No Animation:** Design for static transitions. UI elements should snap or fade instantly to minimize e-ink "ghosting."