# these should be listed in the environment pane
x = 1
y <- 1

tinyplot::tinyplot(Sepal.Length ~ Petal.Length | Species, data = iris)

png(filename = "my_plot.png", width = 800, height = 600, res = 100)
tinyplot::tinyplot(Sepal.Length ~ Petal.Length | Species, data = iris)
dev.off()
