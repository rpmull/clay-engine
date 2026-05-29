namespace ClaymoreEngine
{
    internal static class UIRenderSpaceUtility
    {
        private static Canvas FindNearestCanvas(Entity entity)
        {
            Entity current = entity;
            while (current.IsValid)
            {
                Canvas canvas = current.GetComponent<Canvas>();
                if (canvas != null)
                    return canvas;

                Entity? parent = current.parent;
                if (!parent.HasValue)
                    break;
                current = parent.Value;
            }

            return null;
        }

        internal static bool GetWorldSpace(Entity entity)
        {
            Canvas canvas = FindNearestCanvas(entity);
            return canvas != null && canvas.worldSpace;
        }

        internal static void SetWorldSpace(Entity entity, bool worldSpace)
        {
            if (!entity.IsValid)
                return;

            Canvas canvas = entity.GetComponent<Canvas>();
            if (canvas == null)
            {
                Canvas effectiveCanvas = FindNearestCanvas(entity);
                if (!worldSpace && effectiveCanvas == null)
                {
                    Text standaloneText = entity.GetComponent<Text>();
                    if (standaloneText != null)
                        standaloneText.worldSpace = false;
                    return;
                }

                if (effectiveCanvas != null && effectiveCanvas.worldSpace == worldSpace)
                {
                    Text inheritedText = entity.GetComponent<Text>();
                    if (inheritedText != null)
                        inheritedText.worldSpace = false;
                    return;
                }

                canvas = entity.AddComponent<Canvas>();
                if (worldSpace)
                    canvas.billboard = true;
            }

            canvas.worldSpace = worldSpace;

            Text text = entity.GetComponent<Text>();
            if (text != null)
                text.worldSpace = false;
        }

        internal static bool GetBillboard(Entity entity)
        {
            Canvas canvas = FindNearestCanvas(entity);
            return canvas != null ? canvas.billboard : true;
        }

        internal static void SetBillboard(Entity entity, bool billboard)
        {
            if (!entity.IsValid)
                return;

            Canvas canvas = entity.GetComponent<Canvas>();
            if (canvas == null)
            {
                Canvas effectiveCanvas = FindNearestCanvas(entity);
                if (effectiveCanvas == null || !effectiveCanvas.worldSpace)
                    return;

                if (effectiveCanvas.billboard == billboard)
                    return;

                canvas = entity.AddComponent<Canvas>();
                canvas.worldSpace = true;
            }

            canvas.billboard = billboard;

            Text text = entity.GetComponent<Text>();
            if (text != null && canvas.worldSpace)
                text.worldSpace = false;
        }
    }
}
