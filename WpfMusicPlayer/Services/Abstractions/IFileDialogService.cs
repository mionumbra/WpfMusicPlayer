// SPDX-License-Identifier: MIT

namespace WpfMusicPlayer.Services.Abstractions;

public enum FileDialogLocation
{
    DocumentsLibrary,
    MusicLibrary,
    PicturesLibrary,
    VideosLibrary,
    Desktop,
    Downloads,
    ComputerFolder,
    HomeGroup,
    Objects3D,
    Unspecified
}

public enum FileDialogViewMode
{
    List,
    Thumbnail
}

public interface IFileDialogService
{
    IReadOnlyList<string> MusicFileExtensions { get; }

    Task<string?> PickFileAsync(
        IReadOnlyList<string> extensions,
        FileDialogLocation suggestedStartLocation = FileDialogLocation.DocumentsLibrary,
        FileDialogViewMode viewMode = FileDialogViewMode.List);

    Task<IReadOnlyList<string>> PickFilesAsync(
        IReadOnlyList<string> extensions,
        FileDialogLocation suggestedStartLocation = FileDialogLocation.DocumentsLibrary,
        FileDialogViewMode viewMode = FileDialogViewMode.List);

    Task<string?> SaveFileAsync(
        IReadOnlyDictionary<string, IReadOnlyList<string>> fileTypeChoices,
        string suggestedFileName,
        FileDialogLocation suggestedStartLocation = FileDialogLocation.DocumentsLibrary);

    Task<string?> PickMusicFileAsync();
    Task<IReadOnlyList<string>> PickMusicFilesAsync();
    Task<string?> PickWpplAsync();
    Task<string?> SaveWpplAsync(string suggestedFileName = "playlist");
    Task<string?> PickImageAsync();
}
