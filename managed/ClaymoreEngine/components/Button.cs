using System;
using System.Collections.Generic;

namespace ClaymoreEngine
{
    public sealed class Button : ComponentBase
    {
        public bool worldSpace
        {
            get => UIRenderSpaceUtility.GetWorldSpace(entity);
            set => UIRenderSpaceUtility.SetWorldSpace(entity, value);
        }

        public bool billboard
        {
            get => UIRenderSpaceUtility.GetBillboard(entity);
            set => UIRenderSpaceUtility.SetBillboard(entity, value);
        }

        public event Action OnClicked;
        public event Action OnPressed;
        public event Action OnReleased;
        public event Action OnHovered;
        public event Action OnUnhovered;

        private bool _prevPressed;
        private bool _prevHovered;

        private static readonly HashSet<Button> s_Registry = new();
        private static readonly List<Button> s_Snapshot = new();

        internal static void Register(Button button)
        {
            if (button == null) return;
            s_Registry.Add(button);
        }

        internal static void Unregister(Button button)
        {
            if (button == null) return;
            s_Registry.Remove(button);
        }

        internal static void ClearRegistry()
        {
            s_Registry.Clear();
            s_Snapshot.Clear();
        }

        public static void UpdateAll()
        {
            if (s_Registry.Count == 0) return;
            if (ComponentInterop.UI_ButtonIsHovered == null ||
                ComponentInterop.UI_ButtonIsPressed == null ||
                ComponentInterop.UI_ButtonWasClicked == null)
            {
                return;
            }

            s_Snapshot.Clear();
            s_Snapshot.AddRange(s_Registry);
            foreach (var button in s_Snapshot)
            {
                if (button == null || !button.IsValid)
                {
                    if (button != null) s_Registry.Remove(button);
                    continue;
                }
                if (ComponentInterop.HasComponent != null &&
                    !ComponentInterop.HasComponent(button.entity.EntityID, "ButtonComponent"))
                {
                    s_Registry.Remove(button);
                    continue;
                }

                button.Update();
            }
        }

        /// <summary>
        /// Polls the native button state and fires events.
        /// The engine pumps this each frame; call manually if needed.
        /// </summary>
        public void Update()
        {
            bool hovered = ComponentInterop.UI_ButtonIsHovered(entity.EntityID);
            bool pressed = ComponentInterop.UI_ButtonIsPressed(entity.EntityID);
            bool clicked = ComponentInterop.UI_ButtonWasClicked(entity.EntityID); // consumes click

            if (hovered && !_prevHovered) OnHovered?.Invoke();
            if (!hovered && _prevHovered) OnUnhovered?.Invoke();
            if (pressed && !_prevPressed) OnPressed?.Invoke();
            if (!pressed && _prevPressed) OnReleased?.Invoke();
            if (clicked) OnClicked?.Invoke();

            _prevHovered = hovered;
            _prevPressed = pressed;
        }
    }
}


