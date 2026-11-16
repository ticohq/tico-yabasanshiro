-keepattributes *Annotation*
-keepattributes SourceFile,LineNumberTable
-keep public class * extends java.lang.Exception
-keep class com.crashlytics.** { *; }
-dontwarn com.crashlytics.**
-keep class com.activeandroid.** { *; }
-keep class com.activeandroid.**.** { *; }
-keep class * extends com.activeandroid.Model
-keep class * extends com.activeandroid.serializer.TypeSerializer
-keep public class org.uoyabause.android.YabauseRunnable.** { *; }
-keep class org.uoyabause.android.Yabause.** { *; }
-keepclassmembers class **.Yabause { *; }
-keep class org.uoyabause.android.achievements.RetroAchievementsManager { *; }
-keep class org.uoyabause.android.achievements.RetroAchievementsManager$* { *; }
-keepclassmembers class org.uoyabause.android.achievements.RetroAchievementsManager { *; }

# Explicitly keep JNI callback methods to prevent ProGuard from removing them
-keepclassmembers class org.uoyabause.android.achievements.RetroAchievementsManager {
    public void onAchievementUnlocked(int, java.lang.String, java.lang.String, int, java.lang.String, boolean);
    public void onLeaderboardSubmit(int, java.lang.String, java.lang.String, int);
    public void onRichPresenceUpdate(java.lang.String);
    public void onGameLoadComplete(boolean, java.lang.String);
    public void onLoginComplete(boolean, java.lang.String);
    public void onHttpRequest(java.lang.String, java.lang.String, long);
    public void onLeaderboardTrackerShow(int, java.lang.String);
    public void onLeaderboardTrackerHide(int);
    public void onLeaderboardTrackerUpdate(int, java.lang.String);
    public void onChallengeIndicatorShow(int, java.lang.String, java.lang.String);
    public void onChallengeIndicatorHide(int);
    public void onProgressIndicatorShow();
    public void onProgressIndicatorHide();
    public void onProgressIndicatorUpdate(int, java.lang.String, java.lang.String, java.lang.String, int);
    public void onGamePlacard(java.lang.String, java.lang.String, int, int);
}

-keep class org.uoyabause.android.achievements.RetroAchievementsNotification { *; }
-keep class org.uoyabause.android.auth.RetroAchievementsAuthManager { *; }
-keep class org.uoyabause.android.achievements.AchievementListFragment { *; }
-keep class org.uoyabause.android.achievements.AchievementListFragment$* { *; }
-keep class com.google.protobuf.** { *; }
-dontwarn com.google.protobuf.**

 # Add this global rule
 -keepattributes Signature

 # This rule will properly ProGuard all the model classes in
 # the package com.yourcompany.models. Modify to fit the structure
 # of your app.
 -keepclassmembers class org.uoyabause.android.backup.model.BackupItem {
      *;
 }

# Firebase Realtime Database model classes - Keep all fields for serialization/deserialization
-keep class org.uoyabause.android.backup.model.CloudBackupMetadata { *; }
-keepclassmembers class org.uoyabause.android.backup.model.CloudBackupMetadata { *; }

-keepclassmembers class org.uoyabause.android.cheat.CheatItem {
       *;
  }

# Remove debug and verbose log statements in release builds
-assumenosideeffects class android.util.Log {
    public static *** d(...);
    public static *** v(...);
}

# Firestore data classes - Keep all fields and constructors
-keep class org.uoyabause.android.ReportData { *; }
-keepclassmembers class org.uoyabause.android.ReportData { *; }

# General Firestore rules
-keepclassmembers class * {
    @com.google.firebase.firestore.DocumentId *;
}

# Keep Firestore annotation classes
-keep class com.google.firebase.firestore.DocumentId
-keepattributes RuntimeVisibleAnnotations
-keepattributes RuntimeVisibleParameterAnnotations

# Game list cache data classes - Keep for Gson serialization
-keep class org.uoyabause.android.cache.DirectorySnapshot { *; }
-keep class org.uoyabause.android.cache.GameListCacheMetadata { *; }
-keep class org.uoyabause.android.cache.FileEntry { *; }
-keepclassmembers class org.uoyabause.android.cache.DirectorySnapshot { *; }
-keepclassmembers class org.uoyabause.android.cache.GameListCacheMetadata { *; }
-keepclassmembers class org.uoyabause.android.cache.FileEntry { *; }

# AndroidX Preference library - Keep ListPreference methods for dynamic population
-keep class androidx.preference.ListPreference { *; }
-keep class androidx.preference.SeekBarPreference { *; }
-keepclassmembers class androidx.preference.Preference {
    public void setEntries(java.lang.CharSequence[]);
    public void setEntryValues(java.lang.CharSequence[]);
    public java.lang.CharSequence[] getEntries();
    public java.lang.CharSequence[] getEntryValues();
    public java.lang.CharSequence getEntry();
    public void setSummary(java.lang.CharSequence);
}

# Keep all Fragment subclasses (required for fragment restoration after process death)
-keep class * extends androidx.fragment.app.Fragment { <init>(); }
-keep class * extends androidx.fragment.app.DialogFragment { <init>(); }

# Android Gradle plugin generated rules
-dontwarn android.media.LoudnessCodecController$OnLoudnessCodecUpdateListener
-dontwarn android.media.LoudnessCodecController

# SMB client (smbj): optional Kerberos / GSS / JNDI classes are not on Android
-dontwarn org.ietf.jgss.**
-dontwarn javax.security.auth.kerberos.**
-dontwarn javax.el.**
-dontwarn sun.security.**
-dontwarn org.slf4j.**
-dontwarn org.bouncycastle.**
-keep class com.hierynomus.** { *; }
# The mbassador artifact's package is net.engio.mbassy (not ...mbassador). It
# finds @Handler methods and instantiates its dispatchers by reflection, so R8
# renaming it made every smbj event publish fail with MessageBusException.
-keep class net.engio.mbassy.** { *; }
-keep class com.hierynomus.security.bc.** { *; }
