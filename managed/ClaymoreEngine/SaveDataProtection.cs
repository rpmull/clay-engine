using System;
using System.IO;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;

namespace ClaymoreEngine
{
    /// <summary>
    /// Provides encryption and integrity protection for save data.
    /// Prevents players from manually editing save files to cheat.
    /// </summary>
    public static class SaveDataProtection
    {
        // Default key - in production, derive this from hardware ID, game license, etc.
        // Or let games provide their own key via SetEncryptionKey()
        private static byte[] _encryptionKey = Encoding.UTF8.GetBytes("ClaymoreEngine!!");  // 16 bytes for AES-128
        private static byte[] _hmacKey = Encoding.UTF8.GetBytes("IntegrityCheck!!");        // 16 bytes for HMAC
        
        /// <summary>
        /// Set custom encryption keys. Call this during game initialization.
        /// Keys should be 16, 24, or 32 bytes for AES-128, AES-192, or AES-256.
        /// </summary>
        public static void SetEncryptionKeys(byte[] encryptionKey, byte[] hmacKey)
        {
            if (encryptionKey.Length != 16 && encryptionKey.Length != 24 && encryptionKey.Length != 32)
                throw new ArgumentException("Encryption key must be 16, 24, or 32 bytes");
            
            _encryptionKey = encryptionKey;
            _hmacKey = hmacKey;
        }
        
        /// <summary>
        /// Derive keys from a password/secret. More convenient than raw bytes.
        /// </summary>
        public static void SetEncryptionPassword(string password, string salt = "ClaymoreEngineSalt")
        {
            using var deriveBytes = new Rfc2898DeriveBytes(password, Encoding.UTF8.GetBytes(salt), 10000, HashAlgorithmName.SHA256);
            _encryptionKey = deriveBytes.GetBytes(32);  // AES-256
            _hmacKey = deriveBytes.GetBytes(32);
        }

        #region Simple XOR Obfuscation (Fast, low security)
        
        /// <summary>
        /// Simple XOR obfuscation. Fast but not cryptographically secure.
        /// Good enough to stop casual save editing.
        /// </summary>
        public static byte[] ObfuscateXor(byte[] data, byte[] key = null)
        {
            key ??= _encryptionKey;
            var result = new byte[data.Length];
            for (int i = 0; i < data.Length; i++)
            {
                result[i] = (byte)(data[i] ^ key[i % key.Length]);
            }
            return result;
        }
        
        /// <summary>
        /// Deobfuscate XOR data. Same operation as obfuscate (XOR is symmetric).
        /// </summary>
        public static byte[] DeobfuscateXor(byte[] data, byte[] key = null) => ObfuscateXor(data, key);
        
        #endregion

        #region AES Encryption (Strong security)
        
        /// <summary>
        /// Encrypt data using AES-256-CBC.
        /// </summary>
        public static byte[] Encrypt(byte[] plainData)
        {
            using var aes = Aes.Create();
            aes.Key = _encryptionKey;
            aes.GenerateIV();
            
            using var encryptor = aes.CreateEncryptor();
            var encrypted = encryptor.TransformFinalBlock(plainData, 0, plainData.Length);
            
            // Prepend IV to encrypted data (IV is not secret, just needs to be unique)
            var result = new byte[aes.IV.Length + encrypted.Length];
            Buffer.BlockCopy(aes.IV, 0, result, 0, aes.IV.Length);
            Buffer.BlockCopy(encrypted, 0, result, aes.IV.Length, encrypted.Length);
            
            return result;
        }
        
        /// <summary>
        /// Decrypt AES-256-CBC encrypted data.
        /// </summary>
        public static byte[] Decrypt(byte[] encryptedData)
        {
            using var aes = Aes.Create();
            aes.Key = _encryptionKey;
            
            // Extract IV from beginning of data
            var iv = new byte[16];
            Buffer.BlockCopy(encryptedData, 0, iv, 0, 16);
            aes.IV = iv;
            
            using var decryptor = aes.CreateDecryptor();
            return decryptor.TransformFinalBlock(encryptedData, 16, encryptedData.Length - 16);
        }
        
        #endregion

        #region Encryption + Integrity (Best protection)
        
        /// <summary>
        /// Encrypt data and add HMAC integrity check.
        /// If the file is modified in any way, loading will fail.
        /// </summary>
        public static byte[] EncryptWithIntegrity(byte[] plainData)
        {
            // Encrypt first
            var encrypted = Encrypt(plainData);
            
            // Calculate HMAC of encrypted data
            using var hmac = new HMACSHA256(_hmacKey);
            var hash = hmac.ComputeHash(encrypted);
            
            // Format: [4 bytes: magic] [32 bytes: HMAC] [encrypted data]
            var result = new byte[4 + 32 + encrypted.Length];
            
            // Magic bytes to identify our format
            result[0] = (byte)'C';
            result[1] = (byte)'S';
            result[2] = (byte)'A';
            result[3] = (byte)'V';
            
            Buffer.BlockCopy(hash, 0, result, 4, 32);
            Buffer.BlockCopy(encrypted, 0, result, 36, encrypted.Length);
            
            return result;
        }
        
        /// <summary>
        /// Decrypt data and verify integrity.
        /// Throws if the data has been tampered with.
        /// </summary>
        public static byte[] DecryptWithIntegrity(byte[] protectedData)
        {
            // Check magic bytes
            if (protectedData.Length < 36 ||
                protectedData[0] != 'C' || protectedData[1] != 'S' ||
                protectedData[2] != 'A' || protectedData[3] != 'V')
            {
                throw new InvalidDataException("Invalid save file format");
            }
            
            // Extract HMAC
            var storedHash = new byte[32];
            Buffer.BlockCopy(protectedData, 4, storedHash, 0, 32);
            
            // Extract encrypted data
            var encrypted = new byte[protectedData.Length - 36];
            Buffer.BlockCopy(protectedData, 36, encrypted, 0, encrypted.Length);
            
            // Verify HMAC
            using var hmac = new HMACSHA256(_hmacKey);
            var computedHash = hmac.ComputeHash(encrypted);
            
            if (!CryptographicEquals(storedHash, computedHash))
            {
                throw new CryptographicException("Save file has been tampered with!");
            }
            
            // Decrypt
            return Decrypt(encrypted);
        }
        
        // Constant-time comparison to prevent timing attacks
        private static bool CryptographicEquals(byte[] a, byte[] b)
        {
            if (a.Length != b.Length) return false;
            int diff = 0;
            for (int i = 0; i < a.Length; i++)
                diff |= a[i] ^ b[i];
            return diff == 0;
        }
        
        #endregion

        #region High-Level Save/Load API
        
        /// <summary>
        /// Save an object to an encrypted file.
        /// </summary>
        public static void SaveEncrypted<T>(string filePath, T data, bool withIntegrity = true)
        {
            var json = JsonSerializer.Serialize(data, new JsonSerializerOptions { WriteIndented = false });
            var bytes = Encoding.UTF8.GetBytes(json);
            
            // Compress first (optional, reduces file size)
            var compressed = Compress(bytes);
            
            // Encrypt
            var encrypted = withIntegrity ? EncryptWithIntegrity(compressed) : Encrypt(compressed);
            
            // Write to file
            File.WriteAllBytes(filePath, encrypted);
        }
        
        /// <summary>
        /// Load an object from an encrypted file.
        /// </summary>
        public static T LoadEncrypted<T>(string filePath, bool withIntegrity = true)
        {
            if (!File.Exists(filePath))
                throw new FileNotFoundException("Save file not found", filePath);
            
            var encrypted = File.ReadAllBytes(filePath);
            
            // Decrypt
            var compressed = withIntegrity ? DecryptWithIntegrity(encrypted) : Decrypt(encrypted);
            
            // Decompress
            var bytes = Decompress(compressed);
            
            // Deserialize
            var json = Encoding.UTF8.GetString(bytes);
            return JsonSerializer.Deserialize<T>(json);
        }
        
        /// <summary>
        /// Try to load, returning default if file doesn't exist or is corrupted.
        /// </summary>
        public static bool TryLoadEncrypted<T>(string filePath, out T data, bool withIntegrity = true)
        {
            data = default;
            try
            {
                data = LoadEncrypted<T>(filePath, withIntegrity);
                return true;
            }
            catch (FileNotFoundException)
            {
                return false;
            }
            catch (CryptographicException ex)
            {
                Console.WriteLine($"[SaveData] Tampered save file detected: {ex.Message}");
                return false;
            }
            catch (Exception ex)
            {
                Console.WriteLine($"[SaveData] Failed to load: {ex.Message}");
                return false;
            }
        }
        
        #endregion

        #region Compression (reduces file size)
        
        private static byte[] Compress(byte[] data)
        {
            using var output = new MemoryStream();
            using (var gzip = new System.IO.Compression.GZipStream(output, System.IO.Compression.CompressionLevel.Optimal))
            {
                gzip.Write(data, 0, data.Length);
            }
            return output.ToArray();
        }
        
        private static byte[] Decompress(byte[] data)
        {
            using var input = new MemoryStream(data);
            using var gzip = new System.IO.Compression.GZipStream(input, System.IO.Compression.CompressionMode.Decompress);
            using var output = new MemoryStream();
            gzip.CopyTo(output);
            return output.ToArray();
        }
        
        #endregion
    }
    
    /// <summary>
    /// Example save data structure. Your game would define its own.
    /// </summary>
    /// <example>
    /// [Serializable]
    /// public class PlayerSaveData
    /// {
    ///     public string PlayerName { get; set; }
    ///     public int Level { get; set; }
    ///     public int Gold { get; set; }
    ///     public int Health { get; set; }
    ///     public float[] Position { get; set; }
    ///     public List<string> InventoryItems { get; set; }
    ///     public Dictionary<string, string> DialogueState { get; set; }
    ///     public string QuestState { get; set; }
    /// }
    /// 
    /// // Saving
    /// var save = new PlayerSaveData
    /// {
    ///     PlayerName = "Hero",
    ///     Level = 15,
    ///     Gold = 5000,
    ///     Health = 100,
    ///     Position = new[] { 10.5f, 0f, 25.3f },
    ///     InventoryItems = inventory.GetItemIds(),
    ///     DialogueState = DialogueSystem.GetAllStateForSave(),
    ///     QuestState = QuestSystem.SerializeState()
    /// };
    /// 
    /// SaveDataProtection.SaveEncrypted("saves/slot1.sav", save);
    /// 
    /// // Loading
    /// if (SaveDataProtection.TryLoadEncrypted("saves/slot1.sav", out PlayerSaveData loaded))
    /// {
    ///     // Restore game state
    ///     player.Name = loaded.PlayerName;
    ///     player.Level = loaded.Level;
    ///     // etc...
    ///     DialogueSystem.RestoreStateFromSave(loaded.DialogueState);
    ///     QuestSystem.DeserializeState(loaded.QuestState);
    /// }
    /// </example>
}
